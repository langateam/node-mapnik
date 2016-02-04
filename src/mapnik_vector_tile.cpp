#include "utils.hpp"
#include "mapnik_map.hpp"
#include "mapnik_image.hpp"
#if defined(GRID_RENDERER)
#include "mapnik_grid.hpp"
#endif
#include "mapnik_feature.hpp"
#include "mapnik_cairo_surface.hpp"
#ifdef SVG_RENDERER
#include <mapnik/svg/output/svg_renderer.hpp>
#endif

#include "mapnik_vector_tile.hpp"
#include "vector_tile_compression.hpp"
#include "vector_tile_composite.hpp"
#include "vector_tile_processor.hpp"
#include "vector_tile_projection.hpp"
#include "vector_tile_datasource_pbf.hpp"
#include "vector_tile_geometry_decoder.hpp"
#include "vector_tile_load_tile.hpp"
#include "object_to_container.hpp"

// mapnik
#include <mapnik/agg_renderer.hpp>      // for agg_renderer
#include <mapnik/datasource_cache.hpp>
#include <mapnik/box2d.hpp>
#include <mapnik/feature.hpp>
#include <mapnik/featureset.hpp>
#include <mapnik/feature_kv_iterator.hpp>
#include <mapnik/geometry_is_simple.hpp>
#include <mapnik/geometry_is_valid.hpp>
#include <mapnik/geometry_reprojection.hpp>
#include <mapnik/geom_util.hpp>
#include <mapnik/hit_test_filter.hpp>
#include <mapnik/image_any.hpp>
#include <mapnik/layer.hpp>
#include <mapnik/map.hpp>
#include <mapnik/memory_datasource.hpp>
#include <mapnik/projection.hpp>
#include <mapnik/request.hpp>
#include <mapnik/scale_denominator.hpp>
#include <mapnik/util/geometry_to_geojson.hpp>
#include <mapnik/util/feature_to_geojson.hpp>
#include <mapnik/version.hpp>
#if defined(GRID_RENDERER)
#include <mapnik/grid/grid.hpp>         // for hit_grid, grid
#include <mapnik/grid/grid_renderer.hpp>  // for grid_renderer
#endif
#ifdef HAVE_CAIRO
#include <mapnik/cairo/cairo_renderer.hpp>
#include <cairo.h>
#ifdef CAIRO_HAS_SVG_SURFACE
#include <cairo-svg.h>
#endif // CAIRO_HAS_SVG_SURFACE
#endif

// std
#include <set>                          // for set, etc
#include <sstream>                      // for operator<<, basic_ostream, etc
#include <string>                       // for string, char_traits, etc
#include <exception>                    // for exception
#include <vector>                       // for vector

// protozero
#include <protozero/pbf_reader.hpp>

namespace detail
{

struct p2p_result
{
    explicit p2p_result() :
      distance(-1),
      x_hit(0),
      y_hit(0) {}

    double distance;
    double x_hit;
    double y_hit;
};

struct p2p_distance
{
    p2p_distance(double x, double y)
     : x_(x),
       y_(y) {}

    p2p_result operator() (mapnik::geometry::geometry_empty const& ) const
    {
        // There is no current way that a geometry empty could be returned from a vector tile.
        /* LCOV_EXCL_START */
        p2p_result p2p;
        return p2p;
        /* LCOV_EXCL_STOP */
    }

    p2p_result operator() (mapnik::geometry::point<double> const& geom) const
    {
        p2p_result p2p;
        p2p.x_hit = geom.x;
        p2p.y_hit = geom.y;
        p2p.distance = mapnik::distance(geom.x, geom.y, x_, y_);
        return p2p;
    }
    p2p_result operator() (mapnik::geometry::multi_point<double> const& geom) const
    {
        p2p_result p2p;
        for (auto const& pt : geom)
        {
            p2p_result p2p_sub = operator()(pt);
            if (p2p_sub.distance >= 0 && (p2p.distance < 0 || p2p_sub.distance < p2p.distance))
            {
                p2p.x_hit = p2p_sub.x_hit;
                p2p.y_hit = p2p_sub.y_hit;
                p2p.distance = p2p_sub.distance;
            }
        }
        return p2p;
    }
    p2p_result operator() (mapnik::geometry::line_string<double> const& geom) const
    {
        p2p_result p2p;
        std::size_t num_points = geom.num_points();
        if (num_points > 1)
        {
            for (std::size_t i = 1; i < num_points; ++i)
            {
                auto const& pt0 = geom[i-1];
                auto const& pt1 = geom[i];
                double dist = mapnik::point_to_segment_distance(x_,y_,pt0.x,pt0.y,pt1.x,pt1.y);
                if (dist >= 0 && (p2p.distance < 0 || dist < p2p.distance))
                {
                    p2p.x_hit = pt0.x;
                    p2p.y_hit = pt0.y;
                    p2p.distance = dist;
                }
            }
        }
        return p2p;
    }
    p2p_result operator() (mapnik::geometry::multi_line_string<double> const& geom) const
    {
        p2p_result p2p;
        for (auto const& line: geom)
        {
            p2p_result p2p_sub = operator()(line);
            if (p2p_sub.distance >= 0 && (p2p.distance < 0 || p2p_sub.distance < p2p.distance))
            {
                p2p.x_hit = p2p_sub.x_hit;
                p2p.y_hit = p2p_sub.y_hit;
                p2p.distance = p2p_sub.distance;
            }
        }
        return p2p;
    }
    p2p_result operator() (mapnik::geometry::polygon<double> const& geom) const
    {
        auto const& exterior = geom.exterior_ring;
        std::size_t num_points = exterior.num_points();
        p2p_result p2p;
        if (num_points < 4)
        {
            return p2p;
        }
        bool inside = false;
        for (std::size_t i = 1; i < num_points; ++i)
        {
            auto const& pt0 = exterior[i-1];
            auto const& pt1 = exterior[i];
            // todo - account for tolerance
            if (mapnik::detail::pip(pt0.x,pt0.y,pt1.x,pt1.y,x_,y_))
            {
                inside = !inside;
            }
        }
        if (!inside)
        {
            return p2p;
        }
        for (auto const& ring :  geom.interior_rings)
        {
            std::size_t num_interior_points = ring.size();
            if (num_interior_points < 4)
            {
                continue;
            }
            for (std::size_t j = 1; j < num_interior_points; ++j)
            {
                auto const& pt0 = ring[j-1];
                auto const& pt1 = ring[j];
                if (mapnik::detail::pip(pt0.x,pt0.y,pt1.x,pt1.y,x_,y_))
                {
                    inside=!inside;
                }
            }
        }
        if (inside)
        {
            p2p.distance = 0;
        }
        return p2p;
    }
    p2p_result operator() (mapnik::geometry::multi_polygon<double> const& geom) const
    {
        p2p_result p2p;
        for (auto const& poly: geom)
        {
            p2p_result p2p_sub = operator()(poly);
            if (p2p_sub.distance >= 0 && (p2p.distance < 0 || p2p_sub.distance < p2p.distance))
            {
                p2p.x_hit = p2p_sub.x_hit;
                p2p.y_hit = p2p_sub.y_hit;
                p2p.distance = p2p_sub.distance;
            }
        }
        return p2p;
    }
    p2p_result operator() (mapnik::geometry::geometry_collection<double> const& collection) const
    {
        // There is no current way that a geometry collection could be returned from a vector tile.
        /* LCOV_EXCL_START */
        p2p_result p2p;
        for (auto const& geom: collection)
        {
            p2p_result p2p_sub = mapnik::util::apply_visitor((*this),geom);
            if (p2p_sub.distance >= 0 && (p2p.distance < 0 || p2p_sub.distance < p2p.distance))
            {
                p2p.x_hit = p2p_sub.x_hit;
                p2p.y_hit = p2p_sub.y_hit;
                p2p.distance = p2p_sub.distance;
            }
        }
        return p2p;
        /* LCOV_EXCL_STOP */
    }

    double x_;
    double y_;
};

}

detail::p2p_result path_to_point_distance(mapnik::geometry::geometry<double> const& geom, double x, double y)
{
    return mapnik::util::apply_visitor(detail::p2p_distance(x,y), geom);
}

Nan::Persistent<v8::FunctionTemplate> VectorTile::constructor;

/**
 * A generator for the [Mapbox Vector Tile](https://www.mapbox.com/developers/vector-tiles/)
 * specification of compressed and simplified tiled vector data
 *
 * @name mapnik.VectorTile
 * @class
 * @param {number} z
 * @param {number} x
 * @param {number} y
 * @example
 * var vtile = new mapnik.VectorTile(9,112,195);
 */
void VectorTile::Initialize(v8::Local<v8::Object> target)
{
    Nan::HandleScope scope;

    v8::Local<v8::FunctionTemplate> lcons = Nan::New<v8::FunctionTemplate>(VectorTile::New);
    lcons->InstanceTemplate()->SetInternalFieldCount(1);
    lcons->SetClassName(Nan::New("VectorTile").ToLocalChecked());
    Nan::SetPrototypeMethod(lcons, "render", render);
    Nan::SetPrototypeMethod(lcons, "setData", setData);
    Nan::SetPrototypeMethod(lcons, "setDataSync", setDataSync);
    Nan::SetPrototypeMethod(lcons, "getData", getData);
    Nan::SetPrototypeMethod(lcons, "getDataSync", getDataSync);
    Nan::SetPrototypeMethod(lcons, "addData", addData);
    Nan::SetPrototypeMethod(lcons, "addDataSync", addDataSync);
    Nan::SetPrototypeMethod(lcons, "composite", composite);
    Nan::SetPrototypeMethod(lcons, "compositeSync", compositeSync);
    Nan::SetPrototypeMethod(lcons, "query", query);
    Nan::SetPrototypeMethod(lcons, "queryMany", queryMany);
    Nan::SetPrototypeMethod(lcons, "extent", extent);
    Nan::SetPrototypeMethod(lcons, "bufferedExtent", bufferedExtent);
    Nan::SetPrototypeMethod(lcons, "names", names);
    Nan::SetPrototypeMethod(lcons, "emptyLayers", emptyLayers);
    Nan::SetPrototypeMethod(lcons, "paintedLayers", paintedLayers);
    Nan::SetPrototypeMethod(lcons, "toJSON", toJSON);
    Nan::SetPrototypeMethod(lcons, "toGeoJSON", toGeoJSON);
    Nan::SetPrototypeMethod(lcons, "toGeoJSONSync", toGeoJSONSync);
    Nan::SetPrototypeMethod(lcons, "addGeoJSON", addGeoJSON);
    Nan::SetPrototypeMethod(lcons, "addImage", addImage);
    Nan::SetPrototypeMethod(lcons, "addImageSync", addImageSync);
    Nan::SetPrototypeMethod(lcons, "addImageBuffer", addImageBuffer);
    Nan::SetPrototypeMethod(lcons, "addImageBufferSync", addImageBufferSync);
#if BOOST_VERSION >= 105600
    Nan::SetPrototypeMethod(lcons, "reportGeometrySimplicity", reportGeometrySimplicity);
    Nan::SetPrototypeMethod(lcons, "reportGeometrySimplicitySync", reportGeometrySimplicitySync);
    Nan::SetPrototypeMethod(lcons, "reportGeometryValidity", reportGeometryValidity);
    Nan::SetPrototypeMethod(lcons, "reportGeometryValiditySync", reportGeometryValiditySync);
#endif // BOOST_VERSION >= 105600
    Nan::SetPrototypeMethod(lcons, "painted", painted);
    Nan::SetPrototypeMethod(lcons, "clear", clear);
    Nan::SetPrototypeMethod(lcons, "clearSync", clearSync);
    Nan::SetPrototypeMethod(lcons, "empty", empty);

    // properties
    ATTR(lcons, "tileSize", get_tile_size, set_tile_size);
    ATTR(lcons, "bufferSize", get_buffer_size, set_buffer_size);
    
    target->Set(Nan::New("VectorTile").ToLocalChecked(),lcons->GetFunction());
    constructor.Reset(lcons);
}

VectorTile::VectorTile(int z, 
                       int x, 
                       int y, 
                       std::uint32_t tile_size,
                       std::int32_t buffer_size) :
    Nan::ObjectWrap(),
    tile_(std::make_shared<mapnik::vector_tile_impl::merc_tile>(x, y, z, tile_size, buffer_size))
{
}

// For some reason coverage never seems to be considered here even though
// I have tested it and it does print
/* LCOV_EXCL_START */
VectorTile::~VectorTile()
{ 
}
/* LCOV_EXCL_STOP */

NAN_METHOD(VectorTile::New)
{
    if (!info.IsConstructCall())
    {
        Nan::ThrowError("Cannot call constructor as function, you need to use 'new' keyword");
        return;
    }

    if (info.Length() < 3)
    {
        Nan::ThrowError("please provide a z, x, y");
        return;
    }
    
    if (!info[0]->IsNumber() ||
        !info[1]->IsNumber() ||
        !info[2]->IsNumber())
    {
        Nan::ThrowTypeError("required parameters (z, x, and y) must be a integers");
        return;
    }
    
    int z = info[0]->IntegerValue();
    int x = info[1]->IntegerValue();
    int y = info[2]->IntegerValue();
    if (z < 0 || x < 0 || y < 0)
    {
        Nan::ThrowTypeError("required parameters (z, x, and y) must be greater then or equal to zero");
        return;
    }

    std::uint32_t tile_size = 4096;
    std::int32_t buffer_size = 128;
    v8::Local<v8::Object> options = Nan::New<v8::Object>();
    if (info.Length() > 3)
    {
        if (!info[3]->IsObject())
        {
            Nan::ThrowTypeError("optional fourth argument must be an options object");
            return;
        }
        options = info[3]->ToObject();
        if (options->Has(Nan::New("tile_size").ToLocalChecked()))
        {
            v8::Local<v8::Value> opt = options->Get(Nan::New("tile_size").ToLocalChecked());
            if (!opt->IsNumber())
            {
                Nan::ThrowTypeError("optional arg 'tile_size' must be a number");
                return;
            }
            int tile_size_tmp = opt->IntegerValue();
            if (tile_size_tmp <= 0)
            {
                Nan::ThrowTypeError("optional arg 'tile_size' must be greater then zero");
                return;
            }
            tile_size = tile_size_tmp;
        }
        if (options->Has(Nan::New("buffer_size").ToLocalChecked()))
        {
            v8::Local<v8::Value> opt = options->Get(Nan::New("buffer_size").ToLocalChecked());
            if (!opt->IsNumber())
            {
                Nan::ThrowTypeError("optional arg 'buffer_size' must be a number");
                return;
            }
            buffer_size = opt->IntegerValue();
        }
    }
    if (static_cast<double>(tile_size) + (2 * buffer_size) <= 0)
    {
        Nan::ThrowError("too large of a negative buffer for tilesize");
        return;
    }

    VectorTile* d = new VectorTile(z, x, y, tile_size, buffer_size);

    d->Wrap(info.This());
    info.GetReturnValue().Set(info.This());
    return;
}

void _composite(VectorTile* target_vt,
                std::vector<VectorTile*> & vtiles,
                double scale_factor,
                unsigned offset_x,
                unsigned offset_y,
                double area_threshold,
                bool strictly_simple,
                bool multi_polygon_union,
                mapnik::vector_tile_impl::polygon_fill_type fill_type,
                double scale_denominator,
                bool reencode,
                boost::optional<mapnik::box2d<double>> const& max_extent,
                double simplify_distance,
                bool process_all_rings,
                std::string const& image_format,
                mapnik::scaling_method_e scaling_method,
                std::launch threading_mode)
{
    // create map
    mapnik::Map map(target_vt->tile_size(),target_vt->tile_size(),"+init=epsg:3857");
    if (max_extent)
    {
        map.set_maximum_extent(*max_extent);
    }

    std::vector<mapnik::vector_tile_impl::merc_tile_ptr> merc_vtiles;
    for (VectorTile* vt : vtiles)
    {
        merc_vtiles.push_back(vt->get_tile());
    }

    mapnik::vector_tile_impl::processor ren(map);
    ren.set_fill_type(fill_type);
    ren.set_simplify_distance(simplify_distance);
    ren.set_process_all_rings(process_all_rings);
    ren.set_multi_polygon_union(multi_polygon_union);
    ren.set_strictly_simple(strictly_simple);
    ren.set_area_threshold(area_threshold);
    ren.set_scale_factor(scale_factor);
    ren.set_scaling_method(scaling_method);
    ren.set_image_format(image_format);
    ren.set_threading_mode(threading_mode);

    mapnik::vector_tile_impl::composite(*target_vt->get_tile(),
                                        merc_vtiles,
                                        map,
                                        ren,
                                        scale_denominator,
                                        offset_x,
                                        offset_y,
                                        reencode);
}

/**
 * Composite an array of vector tiles into one vector tile
 *
 * @memberof mapnik.VectorTile
 * @name compositeSync
 * @instance
 * @param {v8::Array<mapnik.VectorTile>} vector tiles
 */
NAN_METHOD(VectorTile::compositeSync)
{
    info.GetReturnValue().Set(_compositeSync(info));
}

v8::Local<v8::Value> VectorTile::_compositeSync(Nan::NAN_METHOD_ARGS_TYPE info)
{
    Nan::EscapableHandleScope scope;
    if (info.Length() < 1 || !info[0]->IsArray()) 
    {
        Nan::ThrowTypeError("must provide an array of VectorTile objects and an optional options object");
        return scope.Escape(Nan::Undefined());
    }
    v8::Local<v8::Array> vtiles = info[0].As<v8::Array>();
    unsigned num_tiles = vtiles->Length();
    if (num_tiles < 1) 
    {
        Nan::ThrowTypeError("must provide an array with at least one VectorTile object and an optional options object");
        return scope.Escape(Nan::Undefined());
    }

    // options needed for re-rendering tiles
    // unclear yet to what extent these need to be user
    // driven, but we expose here to avoid hardcoding
    double scale_factor = 1.0;
    unsigned offset_x = 0;
    unsigned offset_y = 0;
    double area_threshold = 0.1;
    bool strictly_simple = true;
    bool multi_polygon_union = false;
    mapnik::vector_tile_impl::polygon_fill_type fill_type = mapnik::vector_tile_impl::positive_fill;
    double scale_denominator = 0.0;
    bool reencode = false;
    boost::optional<mapnik::box2d<double>> max_extent;
    double simplify_distance = 0.0;
    bool process_all_rings = false;
    std::string image_format = "webp";
    mapnik::scaling_method_e scaling_method = mapnik::SCALING_BILINEAR;
    std::launch threading_mode = std::launch::deferred;

    if (info.Length() > 1) 
    {
        // options object
        if (!info[1]->IsObject())
        {
            Nan::ThrowTypeError("optional second argument must be an options object");
            return scope.Escape(Nan::Undefined());
        }
        v8::Local<v8::Object> options = info[1]->ToObject();
        if (options->Has(Nan::New("area_threshold").ToLocalChecked()))
        {
            v8::Local<v8::Value> area_thres = options->Get(Nan::New("area_threshold").ToLocalChecked());
            if (!area_thres->IsNumber())
            {
                Nan::ThrowTypeError("option 'area_threshold' must be an floating point number");
                return scope.Escape(Nan::Undefined());
            }
            area_threshold = area_thres->NumberValue();
            if (area_threshold < 0.0)
            {
                Nan::ThrowTypeError("option 'area_threshold' can not be negative");
                return scope.Escape(Nan::Undefined());
            }
        }
        if (options->Has(Nan::New("simplify_distance").ToLocalChecked())) 
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("simplify_distance").ToLocalChecked());
            if (!param_val->IsNumber()) 
            {
                Nan::ThrowTypeError("option 'simplify_distance' must be an floating point number");
                return scope.Escape(Nan::Undefined());
            }
            simplify_distance = param_val->NumberValue();
            if (simplify_distance < 0.0)
            {
                Nan::ThrowTypeError("option 'simplify_distance' can not be negative");
                return scope.Escape(Nan::Undefined());
            }
        }
        if (options->Has(Nan::New("strictly_simple").ToLocalChecked()))
        {
            v8::Local<v8::Value> strict_simp = options->Get(Nan::New("strictly_simple").ToLocalChecked());
            if (!strict_simp->IsBoolean())
            {
                Nan::ThrowTypeError("option 'strictly_simple' must be a boolean");
                return scope.Escape(Nan::Undefined());
            }
            strictly_simple = strict_simp->BooleanValue();
        }
        if (options->Has(Nan::New("multi_polygon_union").ToLocalChecked()))
        {
            v8::Local<v8::Value> mpu = options->Get(Nan::New("multi_polygon_union").ToLocalChecked());
            if (!mpu->IsBoolean())
            {
                Nan::ThrowTypeError("option 'multi_polygon_union' must be a boolean");
                return scope.Escape(Nan::Undefined());
            }
            multi_polygon_union = mpu->BooleanValue();
        }
        if (options->Has(Nan::New("fill_type").ToLocalChecked())) 
        {
            v8::Local<v8::Value> ft = options->Get(Nan::New("fill_type").ToLocalChecked());
            if (!ft->IsNumber())
            {
                Nan::ThrowTypeError("optional arg 'fill_type' must be a number");
                return scope.Escape(Nan::Undefined());
            }
            fill_type = static_cast<mapnik::vector_tile_impl::polygon_fill_type>(ft->IntegerValue());
            if (fill_type < 0 || fill_type >= mapnik::vector_tile_impl::polygon_fill_type_max)
            {
                Nan::ThrowTypeError("optional arg 'fill_type' out of possible range");
                return scope.Escape(Nan::Undefined());
            }
        }
        if (options->Has(Nan::New("threading_mode").ToLocalChecked()))
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("threading_mode").ToLocalChecked());
            if (!param_val->IsNumber())
            {
                Nan::ThrowTypeError("option 'threading_mode' must be an unsigned integer");
                return scope.Escape(Nan::Undefined());
            }
            threading_mode = static_cast<std::launch>(param_val->IntegerValue());
            if (threading_mode != std::launch::async && 
                threading_mode != std::launch::deferred && 
                threading_mode != (std::launch::async | std::launch::deferred))
            {
                Nan::ThrowTypeError("optional arg 'threading_mode' is invalid");
                return scope.Escape(Nan::Undefined());
            }
        }
        if (options->Has(Nan::New("scale").ToLocalChecked())) 
        {
            v8::Local<v8::Value> bind_opt = options->Get(Nan::New("scale").ToLocalChecked());
            if (!bind_opt->IsNumber())
            {
                Nan::ThrowTypeError("optional arg 'scale' must be a number");
                return scope.Escape(Nan::Undefined());
            }
            scale_factor = bind_opt->NumberValue();
            if (scale_factor <= 0.0)
            {
                Nan::ThrowTypeError("optional arg 'scale' must be greater then zero");
                return scope.Escape(Nan::Undefined());
            }
        }
        if (options->Has(Nan::New("scale_denominator").ToLocalChecked()))
        {
            v8::Local<v8::Value> bind_opt = options->Get(Nan::New("scale_denominator").ToLocalChecked());
            if (!bind_opt->IsNumber())
            {
                Nan::ThrowTypeError("optional arg 'scale_denominator' must be a number");
                return scope.Escape(Nan::Undefined());
            }
            scale_denominator = bind_opt->NumberValue();
            if (scale_denominator < 0.0)
            {
                Nan::ThrowTypeError("optional arg 'scale_denominator' must be non negative number");
                return scope.Escape(Nan::Undefined());
            }
        }
        if (options->Has(Nan::New("offset_x").ToLocalChecked())) 
        {
            v8::Local<v8::Value> bind_opt = options->Get(Nan::New("offset_x").ToLocalChecked());
            if (!bind_opt->IsNumber())
            {
                Nan::ThrowTypeError("optional arg 'offset_x' must be a number");
                return scope.Escape(Nan::Undefined());
            }
            offset_x = bind_opt->IntegerValue();
        }
        if (options->Has(Nan::New("offset_y").ToLocalChecked())) 
        {
            v8::Local<v8::Value> bind_opt = options->Get(Nan::New("offset_y").ToLocalChecked());
            if (!bind_opt->IsNumber())
            {
                Nan::ThrowTypeError("optional arg 'offset_y' must be a number");
                return scope.Escape(Nan::Undefined());
            }
            offset_y = bind_opt->IntegerValue();
        }
        if (options->Has(Nan::New("reencode").ToLocalChecked())) 
        {
            v8::Local<v8::Value> reencode_opt = options->Get(Nan::New("reencode").ToLocalChecked());
            if (!reencode_opt->IsBoolean())
            {
                Nan::ThrowTypeError("reencode value must be a boolean");
                return scope.Escape(Nan::Undefined());
            }
            reencode = reencode_opt->BooleanValue();
        }
        if (options->Has(Nan::New("max_extent").ToLocalChecked())) 
        {
            v8::Local<v8::Value> max_extent_opt = options->Get(Nan::New("max_extent").ToLocalChecked());
            if (!max_extent_opt->IsArray())
            {
                Nan::ThrowTypeError("max_extent value must be an array of [minx,miny,maxx,maxy]");
                return scope.Escape(Nan::Undefined());
            }
            v8::Local<v8::Array> bbox = max_extent_opt.As<v8::Array>();
            auto len = bbox->Length();
            if (!(len == 4))
            {
                Nan::ThrowTypeError("max_extent value must be an array of [minx,miny,maxx,maxy]");
                return scope.Escape(Nan::Undefined());
            }
            v8::Local<v8::Value> minx = bbox->Get(0);
            v8::Local<v8::Value> miny = bbox->Get(1);
            v8::Local<v8::Value> maxx = bbox->Get(2);
            v8::Local<v8::Value> maxy = bbox->Get(3);
            if (!minx->IsNumber() || !miny->IsNumber() || !maxx->IsNumber() || !maxy->IsNumber())
            {
                Nan::ThrowError("max_extent [minx,miny,maxx,maxy] must be numbers");
                return scope.Escape(Nan::Undefined());
            }
            max_extent = mapnik::box2d<double>(minx->NumberValue(),miny->NumberValue(),
                                               maxx->NumberValue(),maxy->NumberValue());
        }
        if (options->Has(Nan::New("process_all_rings").ToLocalChecked())) 
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("process_all_rings").ToLocalChecked());
            if (!param_val->IsBoolean()) 
            {
                Nan::ThrowTypeError("option 'process_all_rings' must be a boolean");
                return scope.Escape(Nan::Undefined());
            }
            process_all_rings = param_val->BooleanValue();
        }

        if (options->Has(Nan::New("image_scaling").ToLocalChecked())) 
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("image_scaling").ToLocalChecked());
            if (!param_val->IsString()) 
            {
                Nan::ThrowTypeError("option 'image_scaling' must be a string");
                return scope.Escape(Nan::Undefined());
            }
            std::string image_scaling = TOSTR(param_val);
            boost::optional<mapnik::scaling_method_e> method = mapnik::scaling_method_from_string(image_scaling);
            if (!method) 
            {
                Nan::ThrowTypeError("option 'image_scaling' must be a string and a valid scaling method (e.g 'bilinear')");
                return scope.Escape(Nan::Undefined());
            }
            scaling_method = *method;
        }

        if (options->Has(Nan::New("image_format").ToLocalChecked())) 
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("image_format").ToLocalChecked());
            if (!param_val->IsString()) 
            {
                Nan::ThrowTypeError("option 'image_format' must be a string");
                return scope.Escape(Nan::Undefined());
            }
            image_format = TOSTR(param_val);
        }
    }
    VectorTile* target_vt = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    std::vector<VectorTile*> vtiles_vec;
    vtiles_vec.reserve(num_tiles);
    for (unsigned j=0;j < num_tiles;++j) 
    {
        v8::Local<v8::Value> val = vtiles->Get(j);
        if (!val->IsObject()) 
        {
            Nan::ThrowTypeError("must provide an array of VectorTile objects");
            return scope.Escape(Nan::Undefined());
        }
        v8::Local<v8::Object> tile_obj = val->ToObject();
        if (tile_obj->IsNull() || tile_obj->IsUndefined() || !Nan::New(VectorTile::constructor)->HasInstance(tile_obj)) 
        {
            Nan::ThrowTypeError("must provide an array of VectorTile objects");
            return scope.Escape(Nan::Undefined());
        }
        vtiles_vec.push_back(Nan::ObjectWrap::Unwrap<VectorTile>(tile_obj));
    }
    try
    {
        _composite(target_vt,
                   vtiles_vec,
                   scale_factor,
                   offset_x,
                   offset_y,
                   area_threshold,
                   strictly_simple,
                   multi_polygon_union,
                   fill_type,
                   scale_denominator,
                   reencode,
                   max_extent,
                   simplify_distance,
                   process_all_rings,
                   image_format,
                   scaling_method,
                   threading_mode);
    }
    catch (std::exception const& ex)
    {
        Nan::ThrowTypeError(ex.what());
        return scope.Escape(Nan::Undefined());
    }

    return scope.Escape(Nan::Undefined());
}

typedef struct 
{
    uv_work_t request;
    VectorTile* d;
    double scale_factor;
    unsigned offset_x;
    unsigned offset_y;
    double area_threshold;
    double scale_denominator;
    std::vector<VectorTile*> vtiles;
    bool error;
    bool strictly_simple;
    bool multi_polygon_union;
    mapnik::vector_tile_impl::polygon_fill_type fill_type;
    bool reencode;
    boost::optional<mapnik::box2d<double>> max_extent;
    double simplify_distance;
    bool process_all_rings;
    std::string image_format;
    mapnik::scaling_method_e scaling_method;
    std::launch threading_mode;
    std::string error_name;
    Nan::Persistent<v8::Function> cb;
} vector_tile_composite_baton_t;

NAN_METHOD(VectorTile::composite)
{
    if ((info.Length() < 2) || !info[info.Length()-1]->IsFunction()) 
    {
        info.GetReturnValue().Set(_compositeSync(info));
        return;
    }
    if (!info[0]->IsArray()) 
    {
        Nan::ThrowTypeError("must provide an array of VectorTile objects and an optional options object");
        return;
    }
    v8::Local<v8::Array> vtiles = info[0].As<v8::Array>();
    unsigned num_tiles = vtiles->Length();
    if (num_tiles < 1) 
    {
        Nan::ThrowTypeError("must provide an array with at least one VectorTile object and an optional options object");
        return;
    }

    // options needed for re-rendering tiles
    // unclear yet to what extent these need to be user
    // driven, but we expose here to avoid hardcoding
    double scale_factor = 1.0;
    unsigned offset_x = 0;
    unsigned offset_y = 0;
    double area_threshold = 0.1;
    bool strictly_simple = true;
    bool multi_polygon_union = false;
    mapnik::vector_tile_impl::polygon_fill_type fill_type = mapnik::vector_tile_impl::positive_fill;
    double scale_denominator = 0.0;
    bool reencode = false;
    boost::optional<mapnik::box2d<double>> max_extent;
    double simplify_distance = 0.0;
    bool process_all_rings = false;
    std::string image_format = "webp";
    mapnik::scaling_method_e scaling_method = mapnik::SCALING_BILINEAR;
    std::launch threading_mode = std::launch::deferred;
    std::string merc_srs("+init=epsg:3857");

    if (info.Length() > 2) 
    {
        // options object
        if (!info[1]->IsObject())
        {
            Nan::ThrowTypeError("optional second argument must be an options object");
            return;
        }
        v8::Local<v8::Object> options = info[1]->ToObject();
        if (options->Has(Nan::New("area_threshold").ToLocalChecked()))
        {
            v8::Local<v8::Value> area_thres = options->Get(Nan::New("area_threshold").ToLocalChecked());
            if (!area_thres->IsNumber())
            {
                Nan::ThrowTypeError("option 'area_threshold' must be a number");
                return;
            }
            area_threshold = area_thres->NumberValue();
            if (area_threshold < 0.0)
            {
                Nan::ThrowTypeError("option 'area_threshold' can not be negative");
                return;
            }
        }
        if (options->Has(Nan::New("strictly_simple").ToLocalChecked())) 
        {
            v8::Local<v8::Value> strict_simp = options->Get(Nan::New("strictly_simple").ToLocalChecked());
            if (!strict_simp->IsBoolean())
            {
                Nan::ThrowTypeError("strictly_simple value must be a boolean");
                return;
            }
            strictly_simple = strict_simp->BooleanValue();
        }
        if (options->Has(Nan::New("multi_polygon_union").ToLocalChecked())) 
        {
            v8::Local<v8::Value> mpu = options->Get(Nan::New("multi_polygon_union").ToLocalChecked());
            if (!mpu->IsBoolean())
            {
                Nan::ThrowTypeError("multi_polygon_union value must be a boolean");
                return;
            }
            multi_polygon_union = mpu->BooleanValue();
        }
        if (options->Has(Nan::New("fill_type").ToLocalChecked())) 
        {
            v8::Local<v8::Value> ft = options->Get(Nan::New("fill_type").ToLocalChecked());
            if (!ft->IsNumber())
            {
                Nan::ThrowTypeError("optional arg 'fill_type' must be a number");
                return;
            }
            fill_type = static_cast<mapnik::vector_tile_impl::polygon_fill_type>(ft->IntegerValue());
            if (fill_type < 0 || fill_type >= mapnik::vector_tile_impl::polygon_fill_type_max)
            {
                Nan::ThrowTypeError("optional arg 'fill_type' out of possible range");
                return;
            }
        }
        if (options->Has(Nan::New("threading_mode").ToLocalChecked()))
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("threading_mode").ToLocalChecked());
            if (!param_val->IsNumber())
            {
                Nan::ThrowTypeError("option 'threading_mode' must be an unsigned integer");
                return;
            }
            threading_mode = static_cast<std::launch>(param_val->IntegerValue());
            if (threading_mode != std::launch::async && 
                threading_mode != std::launch::deferred &&
                threading_mode != (std::launch::async | std::launch::deferred))
            {
                Nan::ThrowTypeError("optional arg 'threading_mode' is not a valid value");
                return;
            }
        }
        if (options->Has(Nan::New("simplify_distance").ToLocalChecked())) 
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("simplify_distance").ToLocalChecked());
            if (!param_val->IsNumber()) 
            {
                Nan::ThrowTypeError("option 'simplify_distance' must be an floating point number");
                return;
            }
            simplify_distance = param_val->NumberValue();
            if (simplify_distance < 0.0)
            {
                Nan::ThrowTypeError("option 'simplify_distance' can not be negative");
                return;
            }
        }
        if (options->Has(Nan::New("scale").ToLocalChecked())) 
        {
            v8::Local<v8::Value> bind_opt = options->Get(Nan::New("scale").ToLocalChecked());
            if (!bind_opt->IsNumber())
            {
                Nan::ThrowTypeError("optional arg 'scale' must be a number");
                return;
            }
            scale_factor = bind_opt->NumberValue();
            if (scale_factor < 0.0)
            {
                Nan::ThrowTypeError("option 'scale' can not be negative");
                return;
            }
        }
        if (options->Has(Nan::New("scale_denominator").ToLocalChecked()))
        {
            v8::Local<v8::Value> bind_opt = options->Get(Nan::New("scale_denominator").ToLocalChecked());
            if (!bind_opt->IsNumber())
            {
                Nan::ThrowTypeError("optional arg 'scale_denominator' must be a number");
                return;
            }
            scale_denominator = bind_opt->NumberValue();
            if (scale_denominator < 0.0)
            {
                Nan::ThrowTypeError("option 'scale_denominator' can not be negative");
                return;
            }
        }
        if (options->Has(Nan::New("offset_x").ToLocalChecked())) 
        {
            v8::Local<v8::Value> bind_opt = options->Get(Nan::New("offset_x").ToLocalChecked());
            if (!bind_opt->IsNumber())
            {
                Nan::ThrowTypeError("optional arg 'offset_x' must be a number");
                return;
            }
            offset_x = bind_opt->IntegerValue();
        }
        if (options->Has(Nan::New("offset_y").ToLocalChecked())) 
        {
            v8::Local<v8::Value> bind_opt = options->Get(Nan::New("offset_y").ToLocalChecked());
            if (!bind_opt->IsNumber())
            {
                Nan::ThrowTypeError("optional arg 'offset_y' must be a number");
                return;
            }
            offset_y = bind_opt->IntegerValue();
        }
        if (options->Has(Nan::New("reencode").ToLocalChecked())) 
        {
            v8::Local<v8::Value> reencode_opt = options->Get(Nan::New("reencode").ToLocalChecked());
            if (!reencode_opt->IsBoolean())
            {
                Nan::ThrowTypeError("reencode value must be a boolean");
                return;
            }
            reencode = reencode_opt->BooleanValue();
        }
        if (options->Has(Nan::New("max_extent").ToLocalChecked())) 
        {
            v8::Local<v8::Value> max_extent_opt = options->Get(Nan::New("max_extent").ToLocalChecked());
            if (!max_extent_opt->IsArray())
            {
                Nan::ThrowTypeError("max_extent value must be an array of [minx,miny,maxx,maxy]");
                return;
            }
            v8::Local<v8::Array> bbox = max_extent_opt.As<v8::Array>();
            auto len = bbox->Length();
            if (!(len == 4))
            {
                Nan::ThrowTypeError("max_extent value must be an array of [minx,miny,maxx,maxy]");
                return;
            }
            v8::Local<v8::Value> minx = bbox->Get(0);
            v8::Local<v8::Value> miny = bbox->Get(1);
            v8::Local<v8::Value> maxx = bbox->Get(2);
            v8::Local<v8::Value> maxy = bbox->Get(3);
            if (!minx->IsNumber() || !miny->IsNumber() || !maxx->IsNumber() || !maxy->IsNumber())
            {
                Nan::ThrowError("max_extent [minx,miny,maxx,maxy] must be numbers");
                return;
            }
            max_extent = mapnik::box2d<double>(minx->NumberValue(),miny->NumberValue(),
                                               maxx->NumberValue(),maxy->NumberValue());
        }
        if (options->Has(Nan::New("process_all_rings").ToLocalChecked())) 
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("process_all_rings").ToLocalChecked());
            if (!param_val->IsBoolean()) {
                Nan::ThrowTypeError("option 'process_all_rings' must be a boolean");
                return;
            }
            process_all_rings = param_val->BooleanValue();
        }

        if (options->Has(Nan::New("image_scaling").ToLocalChecked())) 
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("image_scaling").ToLocalChecked());
            if (!param_val->IsString()) 
            {
                Nan::ThrowTypeError("option 'image_scaling' must be a string");
                return;
            }
            std::string image_scaling = TOSTR(param_val);
            boost::optional<mapnik::scaling_method_e> method = mapnik::scaling_method_from_string(image_scaling);
            if (!method) 
            {
                Nan::ThrowTypeError("option 'image_scaling' must be a string and a valid scaling method (e.g 'bilinear')");
                return;
            }
            scaling_method = *method;
        }

        if (options->Has(Nan::New("image_format").ToLocalChecked())) 
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("image_format").ToLocalChecked());
            if (!param_val->IsString()) 
            {
                Nan::ThrowTypeError("option 'image_format' must be a string");
                return;
            }
            image_format = TOSTR(param_val);
        }
    }

    v8::Local<v8::Value> callback = info[info.Length()-1];
    vector_tile_composite_baton_t *closure = new vector_tile_composite_baton_t();
    closure->request.data = closure;
    closure->offset_x = offset_x;
    closure->offset_y = offset_y;
    closure->strictly_simple = strictly_simple;
    closure->fill_type = fill_type;
    closure->multi_polygon_union = multi_polygon_union;
    closure->area_threshold = area_threshold;
    closure->scale_factor = scale_factor;
    closure->scale_denominator = scale_denominator;
    closure->reencode = reencode;
    closure->max_extent = max_extent;
    closure->simplify_distance = simplify_distance;
    closure->process_all_rings = process_all_rings;
    closure->scaling_method = scaling_method;
    closure->image_format = image_format;
    closure->threading_mode = threading_mode;
    closure->d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    closure->error = false;
    closure->vtiles.reserve(num_tiles);
    for (unsigned j=0;j < num_tiles;++j)
    {
        v8::Local<v8::Value> val = vtiles->Get(j);
        if (!val->IsObject())
        {
            delete closure;
            Nan::ThrowTypeError("must provide an array of VectorTile objects");
            return;
        }
        v8::Local<v8::Object> tile_obj = val->ToObject();
        if (tile_obj->IsNull() || tile_obj->IsUndefined() || !Nan::New(VectorTile::constructor)->HasInstance(tile_obj))
        {
            delete closure;
            Nan::ThrowTypeError("must provide an array of VectorTile objects");
            return;
        }
        VectorTile* vt = Nan::ObjectWrap::Unwrap<VectorTile>(tile_obj);
        vt->Ref();
        closure->vtiles.push_back(vt);
    }
    closure->d->Ref();
    closure->cb.Reset(callback.As<v8::Function>());
    uv_queue_work(uv_default_loop(), &closure->request, EIO_Composite, (uv_after_work_cb)EIO_AfterComposite);
    return;
}

void VectorTile::EIO_Composite(uv_work_t* req)
{
    vector_tile_composite_baton_t *closure = static_cast<vector_tile_composite_baton_t *>(req->data);
    try
    {
        _composite(closure->d,
                   closure->vtiles,
                   closure->scale_factor,
                   closure->offset_x,
                   closure->offset_y,
                   closure->area_threshold,
                   closure->strictly_simple,
                   closure->multi_polygon_union,
                   closure->fill_type,
                   closure->scale_denominator,
                   closure->reencode,
                   closure->max_extent,
                   closure->simplify_distance,
                   closure->process_all_rings,
                   closure->image_format,
                   closure->scaling_method,
                   closure->threading_mode);
    }
    catch (std::exception const& ex)
    {
        closure->error = true;
        closure->error_name = ex.what();
    }
}

void VectorTile::EIO_AfterComposite(uv_work_t* req)
{
    Nan::HandleScope scope;
    vector_tile_composite_baton_t *closure = static_cast<vector_tile_composite_baton_t *>(req->data);

    if (closure->error)
    {
        v8::Local<v8::Value> argv[1] = { Nan::Error(closure->error_name.c_str()) };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 1, argv);
    }
    else
    {
        v8::Local<v8::Value> argv[2] = { Nan::Null(), closure->d->handle() };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 2, argv);
    }
    for (VectorTile* vt : closure->vtiles)
    {
        vt->Unref();
    }
    closure->d->Unref();
    closure->cb.Reset();
    delete closure;
}

/**
 * Get the extent of this vector tile
 *
 * @memberof mapnik.VectorTile
 * @name extent
 * @instance
 * @param {v8::Array<number>} extent
 */
NAN_METHOD(VectorTile::extent)
{
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    v8::Local<v8::Array> arr = Nan::New<v8::Array>(4);
    mapnik::box2d<double> const& e = d->tile_->extent();
    arr->Set(0, Nan::New<v8::Number>(e.minx()));
    arr->Set(1, Nan::New<v8::Number>(e.miny()));
    arr->Set(2, Nan::New<v8::Number>(e.maxx()));
    arr->Set(3, Nan::New<v8::Number>(e.maxy()));
    info.GetReturnValue().Set(arr);
    return;
}

/**
 * Get the extent including the buffer of this vector tile
 *
 * @memberof mapnik.VectorTile
 * @name bufferedExtent
 * @instance
 * @param {v8::Array<number>} extent
 */
NAN_METHOD(VectorTile::bufferedExtent)
{
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    v8::Local<v8::Array> arr = Nan::New<v8::Array>(4);
    mapnik::box2d<double> e = d->tile_->get_buffered_extent();
    arr->Set(0, Nan::New<v8::Number>(e.minx()));
    arr->Set(1, Nan::New<v8::Number>(e.miny()));
    arr->Set(2, Nan::New<v8::Number>(e.maxx()));
    arr->Set(3, Nan::New<v8::Number>(e.maxy()));
    info.GetReturnValue().Set(arr);
    return;
}

/**
 * Get the names of all of the layers in this vector tile
 *
 * @memberof mapnik.VectorTile
 * @name names
 * @instance
 * @param {v8::Array<string>} layer names
 */
NAN_METHOD(VectorTile::names)
{
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    std::vector<std::string> const& names = d->tile_->get_layers();
    v8::Local<v8::Array> arr = Nan::New<v8::Array>(names.size());
    unsigned idx = 0;
    for (std::string const& name : names)
    {
        arr->Set(idx++,Nan::New<v8::String>(name).ToLocalChecked());
    }
    info.GetReturnValue().Set(arr);
    return;
}

/**
 * Get the names of all of the empty layers in this vector tile
 *
 * @memberof mapnik.VectorTile
 * @name emptyLayers
 * @instance
 * @param {v8::Array<string>} layer names
 */
NAN_METHOD(VectorTile::emptyLayers)
{
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    std::set<std::string> const& names = d->tile_->get_empty_layers();
    v8::Local<v8::Array> arr = Nan::New<v8::Array>(names.size());
    unsigned idx = 0;
    for (std::string const& name : names)
    {
        arr->Set(idx++,Nan::New<v8::String>(name).ToLocalChecked());
    }
    info.GetReturnValue().Set(arr);
    return;
}

/**
 * Get the names of all of the painted layers in this vector tile
 *
 * @memberof mapnik.VectorTile
 * @name paintedLayers
 * @instance
 * @param {v8::Array<string>} layer names
 */
NAN_METHOD(VectorTile::paintedLayers)
{
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    std::set<std::string> const& names = d->tile_->get_painted_layers();
    v8::Local<v8::Array> arr = Nan::New<v8::Array>(names.size());
    unsigned idx = 0;
    for (std::string const& name : names)
    {
        arr->Set(idx++,Nan::New<v8::String>(name).ToLocalChecked());
    }
    info.GetReturnValue().Set(arr);
    return;
}

/**
 * Return whether this vector tile is empty - whether it has no
 * layers and no features
 *
 * @memberof mapnik.VectorTile
 * @name empty
 * @instance
 * @param {boolean} whether the layer is empty
 */
NAN_METHOD(VectorTile::empty)
{
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    info.GetReturnValue().Set(Nan::New<v8::Boolean>(d->tile_->is_empty()));
}

/**
 * Get whether the vector tile has been painted
 *
 * @memberof mapnik.VectorTile
 * @name painted
 * @instance
 * @param {boolean} painted
 */
NAN_METHOD(VectorTile::painted)
{
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    info.GetReturnValue().Set(Nan::New(d->tile_->is_painted()));
}

typedef struct 
{
    uv_work_t request;
    VectorTile* d;
    double lon;
    double lat;
    double tolerance;
    bool error;
    std::vector<query_result> result;
    std::string layer_name;
    std::string error_name;
    Nan::Persistent<v8::Function> cb;
} vector_tile_query_baton_t;

/**
 * Query a vector tile by longitude and latitude
 *
 * @memberof mapnik.VectorTile
 * @name query
 * @instance
 * @param {number} longitude
 * @param {number} latitude
 * @param {Object} [options={tolerance:0}] tolerance: allow results that
 * are not exactly on this longitude, latitude position.
 * @param {Function} callback
 */
NAN_METHOD(VectorTile::query)
{
    if (info.Length() < 2 || !info[0]->IsNumber() || !info[1]->IsNumber())
    {
        Nan::ThrowError("expects lon,lat info");
        return;
    }
    double tolerance = 0.0; // meters
    std::string layer_name("");
    if (info.Length() > 2)
    {
        v8::Local<v8::Object> options = Nan::New<v8::Object>();
        if (!info[2]->IsObject())
        {
            Nan::ThrowTypeError("optional third argument must be an options object");
            return;
        }
        options = info[2]->ToObject();
        if (options->Has(Nan::New("tolerance").ToLocalChecked()))
        {
            v8::Local<v8::Value> tol = options->Get(Nan::New("tolerance").ToLocalChecked());
            if (!tol->IsNumber())
            {
                Nan::ThrowTypeError("tolerance value must be a number");
                return;
            }
            tolerance = tol->NumberValue();
        }
        if (options->Has(Nan::New("layer").ToLocalChecked()))
        {
            v8::Local<v8::Value> layer_id = options->Get(Nan::New("layer").ToLocalChecked());
            if (!layer_id->IsString())
            {
                Nan::ThrowTypeError("layer value must be a string");
                return;
            }
            layer_name = TOSTR(layer_id);
        }
    }

    double lon = info[0]->NumberValue();
    double lat = info[1]->NumberValue();
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());

    // If last argument is not a function go with sync call.
    if (!info[info.Length()-1]->IsFunction())
    {
        try  
        {
            std::vector<query_result> result = _query(d, lon, lat, tolerance, layer_name);
            v8::Local<v8::Array> arr = _queryResultToV8(result);
            info.GetReturnValue().Set(arr);
            return;
        }
        catch (std::exception const& ex)
        {
            Nan::ThrowError(ex.what());
            return;
        }
    } 
    else 
    {
        v8::Local<v8::Value> callback = info[info.Length()-1];
        vector_tile_query_baton_t *closure = new vector_tile_query_baton_t();
        closure->request.data = closure;
        closure->lon = lon;
        closure->lat = lat;
        closure->tolerance = tolerance;
        closure->layer_name = layer_name;
        closure->d = d;
        closure->error = false;
        closure->cb.Reset(callback.As<v8::Function>());
        uv_queue_work(uv_default_loop(), &closure->request, EIO_Query, (uv_after_work_cb)EIO_AfterQuery);
        d->Ref();
        return;
    }
}

void VectorTile::EIO_Query(uv_work_t* req)
{
    vector_tile_query_baton_t *closure = static_cast<vector_tile_query_baton_t *>(req->data);
    try
    {
        closure->result = _query(closure->d, closure->lon, closure->lat, closure->tolerance, closure->layer_name);
    }
    catch (std::exception const& ex)
    {
        closure->error = true;
        closure->error_name = ex.what();
    }
}

void VectorTile::EIO_AfterQuery(uv_work_t* req)
{
    Nan::HandleScope scope;
    vector_tile_query_baton_t *closure = static_cast<vector_tile_query_baton_t *>(req->data);
    if (closure->error)
    {
        v8::Local<v8::Value> argv[1] = { Nan::Error(closure->error_name.c_str()) };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 1, argv);
    }
    else
    {
        std::vector<query_result> result = closure->result;
        v8::Local<v8::Array> arr = _queryResultToV8(result);
        v8::Local<v8::Value> argv[2] = { Nan::Null(), arr };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 2, argv);
    }

    closure->d->Unref();
    closure->cb.Reset();
    delete closure;
}

std::vector<query_result> VectorTile::_query(VectorTile* d, double lon, double lat, double tolerance, std::string const& layer_name)
{
    std::vector<query_result> arr;
    if (d->tile_->is_empty())
    {
        return arr;
    }

    mapnik::projection wgs84("+init=epsg:4326",true);
    mapnik::projection merc("+init=epsg:3857",true);
    mapnik::proj_transform tr(wgs84,merc);
    double x = lon;
    double y = lat;
    double z = 0;
    if (!tr.forward(x,y,z))
    {
        // THIS CAN NEVER BE REACHED CURRENTLY
        // internally lonlat2merc in mapnik can never return false.
        /* LCOV_EXCL_START */
        throw std::runtime_error("could not reproject lon/lat to mercator");
        /* LCOV_EXCL_STOP */
    }

    mapnik::coord2d pt(x,y);
    if (!layer_name.empty())
    {
        protozero::pbf_reader layer_msg;
        if (d->tile_->layer_reader(layer_name, layer_msg))
        {
            auto ds = std::make_shared<mapnik::vector_tile_impl::tile_datasource_pbf>(
                                            layer_msg,
                                            d->tile_->x(),
                                            d->tile_->y(),
                                            d->tile_->z());
            mapnik::featureset_ptr fs = ds->features_at_point(pt, tolerance);
            if (fs)
            {
                mapnik::feature_ptr feature;
                while ((feature = fs->next()))
                {
                    auto const& geom = feature->get_geometry();
                    auto p2p = path_to_point_distance(geom,x,y);
                    if (!tr.backward(p2p.x_hit,p2p.y_hit,z))
                    {
                        /* LCOV_EXCL_START */
                        throw std::runtime_error("could not reproject lon/lat to mercator");
                        /* LCOV_EXCL_STOP */
                    }
                    if (p2p.distance >= 0 && p2p.distance <= tolerance)
                    {
                        query_result res;
                        res.x_hit = p2p.x_hit;
                        res.y_hit = p2p.y_hit;
                        res.distance = p2p.distance;
                        res.layer = layer_name;
                        res.feature = feature;
                        arr.push_back(std::move(res));
                    }
                }
            }
        }
    }
    else
    {
        protozero::pbf_reader item(d->tile_->get_reader());
        while (item.next(3))
        {
            protozero::pbf_reader layer_msg = item.get_message();
            auto ds = std::make_shared<mapnik::vector_tile_impl::tile_datasource_pbf>(
                                            layer_msg,
                                            d->tile_->x(),
                                            d->tile_->y(),
                                            d->tile_->z());
            mapnik::featureset_ptr fs = ds->features_at_point(pt,tolerance);
            if (fs)
            {
                mapnik::feature_ptr feature;
                while ((feature = fs->next()))
                {
                    auto const& geom = feature->get_geometry();
                    auto p2p = path_to_point_distance(geom,x,y);
                    if (!tr.backward(p2p.x_hit,p2p.y_hit,z))
                    {
                        /* LCOV_EXCL_START */
                        throw std::runtime_error("could not reproject lon/lat to mercator");
                        /* LCOV_EXCL_STOP */
                    }
                    if (p2p.distance >= 0 && p2p.distance <= tolerance)
                    {
                        query_result res;
                        res.x_hit = p2p.x_hit;
                        res.y_hit = p2p.y_hit;
                        res.distance = p2p.distance;
                        res.layer = ds->get_name();
                        res.feature = feature;
                        arr.push_back(std::move(res));
                    }
                }
            }
        }
    }
    std::sort(arr.begin(), arr.end(), _querySort);
    return arr;
}

bool VectorTile::_querySort(query_result const& a, query_result const& b)
{
    return a.distance < b.distance;
}

v8::Local<v8::Array> VectorTile::_queryResultToV8(std::vector<query_result> const& result)
{
    v8::Local<v8::Array> arr = Nan::New<v8::Array>();
    std::size_t i = 0;
    for (auto const& item : result)
    {
        v8::Local<v8::Value> feat = Feature::NewInstance(item.feature);
        v8::Local<v8::Object> feat_obj = feat->ToObject();
        feat_obj->Set(Nan::New("layer").ToLocalChecked(),Nan::New<v8::String>(item.layer).ToLocalChecked());
        feat_obj->Set(Nan::New("distance").ToLocalChecked(),Nan::New<v8::Number>(item.distance));
        feat_obj->Set(Nan::New("x_hit").ToLocalChecked(),Nan::New<v8::Number>(item.x_hit));
        feat_obj->Set(Nan::New("y_hit").ToLocalChecked(),Nan::New<v8::Number>(item.y_hit));
        arr->Set(i++,feat);
    }
    return arr;
}

typedef struct
{
    uv_work_t request;
    VectorTile* d;
    std::vector<query_lonlat> query;
    double tolerance;
    std::string layer_name;
    std::vector<std::string> fields;
    queryMany_result result;
    bool error;
    std::string error_name;
    Nan::Persistent<v8::Function> cb;
} vector_tile_queryMany_baton_t;

NAN_METHOD(VectorTile::queryMany)
{
    if (info.Length() < 2 || !info[0]->IsArray())
    {
        Nan::ThrowError("expects lon,lat info + object with layer property referring to a layer name");
        return;
    }

    double tolerance = 0.0; // meters
    std::string layer_name("");
    std::vector<std::string> fields;
    std::vector<query_lonlat> query;

    // Convert v8 queryArray to a std vector
    v8::Local<v8::Array> queryArray = v8::Local<v8::Array>::Cast(info[0]);
    query.reserve(queryArray->Length());
    for (uint32_t p = 0; p < queryArray->Length(); ++p)
    {
        v8::Local<v8::Value> item = queryArray->Get(p);
        if (!item->IsArray())
        {
            Nan::ThrowError("non-array item encountered");
            return;
        }
        v8::Local<v8::Array> pair = v8::Local<v8::Array>::Cast(item);
        v8::Local<v8::Value> lon = pair->Get(0);
        v8::Local<v8::Value> lat = pair->Get(1);
        if (!lon->IsNumber() || !lat->IsNumber())
        {
            Nan::ThrowError("lng lat must be numbers");
            return;
        }
        query_lonlat lonlat;
        lonlat.lon = lon->NumberValue();
        lonlat.lat = lat->NumberValue();
        query.push_back(std::move(lonlat));
    }

    // Convert v8 options object to std params
    if (info.Length() > 1)
    {
        v8::Local<v8::Object> options = Nan::New<v8::Object>();
        if (!info[1]->IsObject())
        {
            Nan::ThrowTypeError("optional second argument must be an options object");
            return;
        }
        options = info[1]->ToObject();
        if (options->Has(Nan::New("tolerance").ToLocalChecked()))
        {
            v8::Local<v8::Value> tol = options->Get(Nan::New("tolerance").ToLocalChecked());
            if (!tol->IsNumber())
            {
                Nan::ThrowTypeError("tolerance value must be a number");
                return;
            }
            tolerance = tol->NumberValue();
        }
        if (options->Has(Nan::New("layer").ToLocalChecked()))
        {
            v8::Local<v8::Value> layer_id = options->Get(Nan::New("layer").ToLocalChecked());
            if (!layer_id->IsString())
            {
                Nan::ThrowTypeError("layer value must be a string");
                return;
            }
            layer_name = TOSTR(layer_id);
        }
        if (options->Has(Nan::New("fields").ToLocalChecked()))
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("fields").ToLocalChecked());
            if (!param_val->IsArray())
            {
                Nan::ThrowTypeError("option 'fields' must be an array of strings");
                return;
            }
            v8::Local<v8::Array> a = v8::Local<v8::Array>::Cast(param_val);
            unsigned int i = 0;
            unsigned int num_fields = a->Length();
            fields.reserve(num_fields);
            while (i < num_fields)
            {
                v8::Local<v8::Value> name = a->Get(i);
                if (name->IsString())
                {
                    fields.emplace_back(TOSTR(name));
                }
                ++i;
            }
        }
    }

    if (layer_name.empty())
    {
        Nan::ThrowTypeError("options.layer is required");
        return;
    }

    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.This());

    // If last argument is not a function go with sync call.
    if (!info[info.Length()-1]->IsFunction())
    {
        try
        {
            queryMany_result result;
            _queryMany(result, d, query, tolerance, layer_name, fields);
            v8::Local<v8::Object> result_obj = _queryManyResultToV8(result);
            info.GetReturnValue().Set(result_obj);
            return;
        }
        catch (std::exception const& ex)
        {
            Nan::ThrowError(ex.what());
            return;
        }
    }
    else
    {
        v8::Local<v8::Value> callback = info[info.Length()-1];
        vector_tile_queryMany_baton_t *closure = new vector_tile_queryMany_baton_t();
        closure->d = d;
        closure->query = query;
        closure->tolerance = tolerance;
        closure->layer_name = layer_name;
        closure->fields = fields;
        closure->error = false;
        closure->request.data = closure;
        closure->cb.Reset(callback.As<v8::Function>());
        uv_queue_work(uv_default_loop(), &closure->request, EIO_QueryMany, (uv_after_work_cb)EIO_AfterQueryMany);
        d->Ref();
        return;
    }
}

void VectorTile::_queryMany(queryMany_result & result, 
                            VectorTile* d, 
                            std::vector<query_lonlat> const& query, 
                            double tolerance, 
                            std::string const& layer_name, 
                            std::vector<std::string> const& fields)
{
    protozero::pbf_reader layer_msg;
    if (!d->tile_->layer_reader(layer_name,layer_msg))
    {
        throw std::runtime_error("Could not find layer in vector tile");
    }
    
    std::map<unsigned,query_result> features;
    std::map<unsigned,std::vector<query_hit> > hits;

    // Reproject query => mercator points
    mapnik::box2d<double> bbox;
    mapnik::projection wgs84("+init=epsg:4326",true);
    mapnik::projection merc("+init=epsg:3857",true);
    mapnik::proj_transform tr(wgs84,merc);
    std::vector<mapnik::coord2d> points;
    points.reserve(query.size());
    for (std::size_t p = 0; p < query.size(); ++p)
    {
        double x = query[p].lon;
        double y = query[p].lat;
        double z = 0;
        if (!tr.forward(x,y,z))
        {
            /* LCOV_EXCL_START */
            throw std::runtime_error("could not reproject lon/lat to mercator");
            /* LCOV_EXCL_STOP */
        }
        mapnik::coord2d pt(x,y);
        bbox.expand_to_include(pt);
        points.emplace_back(std::move(pt));
    }
    bbox.pad(tolerance);

    std::shared_ptr<mapnik::vector_tile_impl::tile_datasource_pbf> ds = std::make_shared<
                                mapnik::vector_tile_impl::tile_datasource_pbf>(
                                    layer_msg,
                                    d->tile_->x(),
                                    d->tile_->y(),
                                    d->tile_->z());
    mapnik::query q(bbox);
    if (fields.empty())
    {
        // request all data attributes
        auto fields2 = ds->get_descriptor().get_descriptors();
        for (auto const& field : fields2)
        {
            q.add_property_name(field.get_name());
        }
    }
    else
    {
        for (std::string const& name : fields)
        {
            q.add_property_name(name);
        }
    }
    mapnik::featureset_ptr fs = ds->features(q);

    if (fs)
    {
        mapnik::feature_ptr feature;
        unsigned idx = 0;
        while ((feature = fs->next()))
        {
            unsigned has_hit = 0;
            for (std::size_t p = 0; p < points.size(); ++p)
            {
                mapnik::coord2d const& pt = points[p];
                auto const& geom = feature->get_geometry();
                auto p2p = path_to_point_distance(geom,pt.x,pt.y);
                if (p2p.distance >= 0 && p2p.distance <= tolerance)
                {
                    has_hit = 1;
                    query_result res;
                    res.feature = feature;
                    res.distance = 0;
                    res.layer = ds->get_name();

                    query_hit hit;
                    hit.distance = p2p.distance;
                    hit.feature_id = idx;

                    features.insert(std::make_pair(idx, res));

                    std::map<unsigned,std::vector<query_hit> >::iterator hits_it;
                    hits_it = hits.find(p);
                    if (hits_it == hits.end())
                    {
                        std::vector<query_hit> pointHits;
                        pointHits.reserve(1);
                        pointHits.push_back(std::move(hit));
                        hits.insert(std::make_pair(p, pointHits));
                    }
                    else
                    {
                        hits_it->second.push_back(std::move(hit));
                    }
                }
            }
            if (has_hit > 0)
            {
                idx++;
            }
        }
    }

    // Sort each group of hits by distance.
    for (auto & hit : hits)
    {
        std::sort(hit.second.begin(), hit.second.end(), _queryManySort);
    }

    result.hits = std::move(hits);
    result.features = std::move(features);
    return;
}

bool VectorTile::_queryManySort(query_hit const& a, query_hit const& b)
{
    return a.distance < b.distance;
}

v8::Local<v8::Object> VectorTile::_queryManyResultToV8(queryMany_result const& result)
{
    v8::Local<v8::Object> results = Nan::New<v8::Object>();
    v8::Local<v8::Array> features = Nan::New<v8::Array>();
    v8::Local<v8::Array> hits = Nan::New<v8::Array>();
    results->Set(Nan::New("hits").ToLocalChecked(), hits);
    results->Set(Nan::New("features").ToLocalChecked(), features);

    // result.features => features
    for (auto const& item : result.features)
    {
        v8::Local<v8::Value> feat = Feature::NewInstance(item.second.feature);
        v8::Local<v8::Object> feat_obj = feat->ToObject();
        feat_obj->Set(Nan::New("layer").ToLocalChecked(),Nan::New<v8::String>(item.second.layer).ToLocalChecked());
        features->Set(item.first, feat_obj);
    }

    // result.hits => hits
    for (auto const& hit : result.hits)
    {
        v8::Local<v8::Array> point_hits = Nan::New<v8::Array>(hit.second.size());
        std::size_t i = 0;
        for (auto const& h : hit.second)
        {
            v8::Local<v8::Object> hit_obj = Nan::New<v8::Object>();
            hit_obj->Set(Nan::New("distance").ToLocalChecked(), Nan::New<v8::Number>(h.distance));
            hit_obj->Set(Nan::New("feature_id").ToLocalChecked(), Nan::New<v8::Number>(h.feature_id));
            point_hits->Set(i++, hit_obj);
        }
        hits->Set(hit.first, point_hits);
    }

    return results;
}

void VectorTile::EIO_QueryMany(uv_work_t* req)
{
    vector_tile_queryMany_baton_t *closure = static_cast<vector_tile_queryMany_baton_t *>(req->data);
    try
    {
        _queryMany(closure->result, closure->d, closure->query, closure->tolerance, closure->layer_name, closure->fields);
    }
    catch (std::exception const& ex)
    {
        closure->error = true;
        closure->error_name = ex.what();
    }
}

void VectorTile::EIO_AfterQueryMany(uv_work_t* req)
{
    Nan::HandleScope scope;
    vector_tile_queryMany_baton_t *closure = static_cast<vector_tile_queryMany_baton_t *>(req->data);
    if (closure->error)
    {
        v8::Local<v8::Value> argv[1] = { Nan::Error(closure->error_name.c_str()) };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 1, argv);
    }
    else
    {
        queryMany_result result = closure->result;
        v8::Local<v8::Object> obj = _queryManyResultToV8(result);
        v8::Local<v8::Value> argv[2] = { Nan::Null(), obj };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 2, argv);
    }

    closure->d->Unref();
    closure->cb.Reset();
    delete closure;
}

struct geometry_type_name
{
    template <typename T>
    std::string operator () (T const& geom) const
    {
        return mapnik::util::apply_visitor(*this, geom);
    }

    std::string operator() (mapnik::geometry::geometry_empty const& ) const
    {
        // LCOV_EXCL_START
        return "Empty";
        // LCOV_EXCL_STOP
    }

    template <typename T>
    std::string operator () (mapnik::geometry::point<T> const&) const
    {
        return "Point";
    }

    template <typename T>
    std::string operator () (mapnik::geometry::line_string<T> const&) const
    {
        return "LineString";
    }

    template <typename T>
    std::string operator () (mapnik::geometry::polygon<T> const&) const
    {
        return "Polygon";
    }

    template <typename T>
    std::string operator () (mapnik::geometry::multi_point<T> const&) const
    {
        return "MultiPoint";
    }

    template <typename T>
    std::string operator () (mapnik::geometry::multi_line_string<T> const&) const
    {
        return "MultiLineString";
    }

    template <typename T>
    std::string operator () (mapnik::geometry::multi_polygon<T> const&) const
    {
        return "MultiPolygon";
    }

    template <typename T>
    std::string operator () (mapnik::geometry::geometry_collection<T> const&) const
    {
        // LCOV_EXCL_START
        return "GeometryCollection";
        // LCOV_EXCL_STOP
    }
};

template <typename T>
static inline std::string geometry_type_as_string(T const& geom)
{
    return geometry_type_name()(geom);
}

struct geometry_array_visitor
{
    v8::Local<v8::Array> operator() (mapnik::geometry::geometry_empty const &)
    {
        // Removed as it should be a bug if a vector tile has reached this point
        // therefore no known tests reach this point
        // LCOV_EXCL_START
        Nan::EscapableHandleScope scope;
        return scope.Escape(Nan::New<v8::Array>());   
        // LCOV_EXCL_STOP
    }
    
    template <typename T>
    v8::Local<v8::Array> operator() (mapnik::geometry::point<T> const & geom)
    {
        Nan::EscapableHandleScope scope;
        v8::Local<v8::Array> arr = Nan::New<v8::Array>(2);
        arr->Set(0, Nan::New<v8::Number>(geom.x));
        arr->Set(1, Nan::New<v8::Number>(geom.y));
        return scope.Escape(arr);   
    }
    
    template <typename T>
    v8::Local<v8::Array> operator() (mapnik::geometry::line_string<T> const & geom)
    {
        Nan::EscapableHandleScope scope;
        if (geom.empty())
        {
            // Removed as it should be a bug if a vector tile has reached this point
            // therefore no known tests reach this point
            // LCOV_EXCL_START
            return scope.Escape(Nan::New<v8::Array>());
            // LCOV_EXCL_STOP
        }
        v8::Local<v8::Array> arr = Nan::New<v8::Array>(geom.size());
        std::uint32_t c = 0;
        for (auto const & pt : geom)
        {
            arr->Set(c++, (*this)(pt));
        }
        return scope.Escape(arr);   
    }
    
    template <typename T>
    v8::Local<v8::Array> operator() (mapnik::geometry::linear_ring<T> const & geom)
    {
        Nan::EscapableHandleScope scope;
        if (geom.empty())
        {
            // Removed as it should be a bug if a vector tile has reached this point
            // therefore no known tests reach this point
            // LCOV_EXCL_START
            return scope.Escape(Nan::New<v8::Array>());
            // LCOV_EXCL_STOP
        }
        v8::Local<v8::Array> arr = Nan::New<v8::Array>(geom.size());
        std::uint32_t c = 0;
        for (auto const & pt : geom)
        {
            arr->Set(c++, (*this)(pt));
        }
        return scope.Escape(arr);   
    }
    
    template <typename T>
    v8::Local<v8::Array> operator() (mapnik::geometry::multi_point<T> const & geom)
    {
        Nan::EscapableHandleScope scope;
        if (geom.empty())
        {
            // Removed as it should be a bug if a vector tile has reached this point
            // therefore no known tests reach this point
            // LCOV_EXCL_START
            return scope.Escape(Nan::New<v8::Array>());
            // LCOV_EXCL_STOP
        }
        v8::Local<v8::Array> arr = Nan::New<v8::Array>(geom.size());
        std::uint32_t c = 0;
        for (auto const & pt : geom)
        {
            arr->Set(c++, (*this)(pt));
        }
        return scope.Escape(arr);   
    }
    
    template <typename T>
    v8::Local<v8::Array> operator() (mapnik::geometry::multi_line_string<T> const & geom)
    {
        Nan::EscapableHandleScope scope;
        if (geom.empty())
        {
            // Removed as it should be a bug if a vector tile has reached this point
            // therefore no known tests reach this point
            // LCOV_EXCL_START
            return scope.Escape(Nan::New<v8::Array>());
            // LCOV_EXCL_STOP
        }
        v8::Local<v8::Array> arr = Nan::New<v8::Array>(geom.size());
        std::uint32_t c = 0;
        for (auto const & pt : geom)
        {
            arr->Set(c++, (*this)(pt));
        }
        return scope.Escape(arr);   
    }
    
    template <typename T>
    v8::Local<v8::Array> operator() (mapnik::geometry::polygon<T> const & geom)
    {
        Nan::EscapableHandleScope scope;
        if (geom.exterior_ring.empty())
        {
            // Removed as it should be a bug if a vector tile has reached this point
            // therefore no known tests reach this point
            // LCOV_EXCL_START
            return scope.Escape(Nan::New<v8::Array>());
            // LCOV_EXCL_STOP
        }
        v8::Local<v8::Array> arr = Nan::New<v8::Array>(1 + geom.interior_rings.size());
        std::uint32_t c = 0;
        arr->Set(c++, (*this)(geom.exterior_ring));
        for (auto const & pt : geom.interior_rings)
        {
            arr->Set(c++, (*this)(pt));
        }
        return scope.Escape(arr);   
    }
    
    template <typename T>
    v8::Local<v8::Array> operator() (mapnik::geometry::multi_polygon<T> const & geom)
    {
        Nan::EscapableHandleScope scope;
        if (geom.empty())
        {
            // Removed as it should be a bug if a vector tile has reached this point
            // therefore no known tests reach this point
            // LCOV_EXCL_START
            return scope.Escape(Nan::New<v8::Array>());
            // LCOV_EXCL_STOP
        }
        v8::Local<v8::Array> arr = Nan::New<v8::Array>(geom.size());
        std::uint32_t c = 0;
        for (auto const & pt : geom)
        {
            arr->Set(c++, (*this)(pt));
        }
        return scope.Escape(arr);   
    }

    template <typename T>
    v8::Local<v8::Array> operator() (mapnik::geometry::geometry<T> const & geom)
    {
        // Removed as it should be a bug if a vector tile has reached this point
        // therefore no known tests reach this point
        // LCOV_EXCL_START
        Nan::EscapableHandleScope scope;
        return scope.Escape(mapnik::util::apply_visitor((*this), geom));
        // LCOV_EXCL_STOP
    }
    
    template <typename T>
    v8::Local<v8::Array> operator() (mapnik::geometry::geometry_collection<T> const & geom)
    {
        // Removed as it should be a bug if a vector tile has reached this point
        // therefore no known tests reach this point
        // LCOV_EXCL_START
        Nan::EscapableHandleScope scope;
        if (geom.empty())
        {
            return scope.Escape(Nan::New<v8::Array>());
        }
        v8::Local<v8::Array> arr = Nan::New<v8::Array>(geom.size());
        std::uint32_t c = 0;
        for (auto const & pt : geom)
        {
            arr->Set(c++, (*this)(pt));
        }
        return scope.Escape(arr);   
        // LCOV_EXCL_STOP
    }
};

template <typename T>
v8::Local<v8::Array> geometry_to_array(mapnik::geometry::geometry<T> const & geom)
{
    Nan::EscapableHandleScope scope;
    return scope.Escape(mapnik::util::apply_visitor(geometry_array_visitor(), geom));
}

struct json_value_visitor
{
    v8::Local<v8::Object> & att_obj_;
    std::string const& name_;

    json_value_visitor(v8::Local<v8::Object> & att_obj,
                       std::string const& name)
        : att_obj_(att_obj),
          name_(name) {}
    
    void operator() (std::string const& val)
    {
        att_obj_->Set(Nan::New(name_).ToLocalChecked(), Nan::New(val).ToLocalChecked());
    }

    void operator() (bool const& val)
    {
        att_obj_->Set(Nan::New(name_).ToLocalChecked(), Nan::New<v8::Boolean>(val));
    }

    void operator() (int64_t const& val)
    {
        att_obj_->Set(Nan::New(name_).ToLocalChecked(), Nan::New<v8::Number>(val));
    }

    void operator() (uint64_t const& val)
    {
        att_obj_->Set(Nan::New(name_).ToLocalChecked(), Nan::New<v8::Number>(val));
    }

    void operator() (double const& val)
    {
        att_obj_->Set(Nan::New(name_).ToLocalChecked(), Nan::New<v8::Number>(val));
    }

    void operator() (float const& val)
    {
        att_obj_->Set(Nan::New(name_).ToLocalChecked(), Nan::New<v8::Number>(val));
    }
};

/**
 * Get a JSON representation of this tile
 *
 * @memberof mapnik.VectorTile
 * @name toJSON
 * @instance
 * @returns {Object} json representation of this tile with name, extent,
 * and version properties
 */
NAN_METHOD(VectorTile::toJSON)
{
    bool decode_geometry = false;
    if (info.Length() >= 1) 
    {
        if (!info[0]->IsObject()) 
        {
            Nan::ThrowError("The first argument must be an object");
            return;
        }
        v8::Local<v8::Object> options = info[0]->ToObject();

        if (options->Has(Nan::New("decode_geometry").ToLocalChecked()))
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("decode_geometry").ToLocalChecked());
            if (!param_val->IsBoolean())
            {
                Nan::ThrowError("option 'decode_geometry' must be a boolean");
                return;
            }
            decode_geometry = param_val->BooleanValue();
        }
    }

    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    try
    {
        protozero::pbf_reader tile_msg = d->tile_->get_reader();
        v8::Local<v8::Array> arr = Nan::New<v8::Array>(d->tile_->get_layers().size());
        std::size_t l_idx = 0;
        while (tile_msg.next(3))
        {
            protozero::pbf_reader layer_msg = tile_msg.get_message();
            v8::Local<v8::Object> layer_obj = Nan::New<v8::Object>();
            std::vector<std::string> layer_keys;
            mapnik::vector_tile_impl::layer_pbf_attr_type layer_values;
            std::vector<protozero::pbf_reader> layer_features;
            protozero::pbf_reader val_msg;
            std::uint32_t version = 1;
            while (layer_msg.next())
            {
                switch (layer_msg.tag())
                {
                    case 1: // name
                        layer_obj->Set(Nan::New("name").ToLocalChecked(), Nan::New<v8::String>(layer_msg.get_string()).ToLocalChecked());
                        break;
                    case 2: // feature
                        layer_features.push_back(layer_msg.get_message());
                        break;
                    case 3: // keys
                        layer_keys.push_back(layer_msg.get_string());
                        break;
                    case 4: // values
                        val_msg = layer_msg.get_message();
                        while (val_msg.next()) 
                        {
                            switch(val_msg.tag())
                            {
                                case 1:
                                    layer_values.push_back(val_msg.get_string());
                                    break;
                                case 2:
                                    layer_values.push_back(val_msg.get_float());
                                    break;
                                case 3:
                                    layer_values.push_back(val_msg.get_double());
                                    break;
                                case 4:
                                    layer_values.push_back(val_msg.get_int64());
                                    break;
                                case 5:
                                    layer_values.push_back(val_msg.get_uint64());
                                    break;
                                case 6:
                                    layer_values.push_back(val_msg.get_sint64());
                                    break;
                                case 7:
                                    layer_values.push_back(val_msg.get_bool());
                                    break;
                                default:
                                    val_msg.skip();
                                    break;
                            }
                        }
                        break;
                    case 5: // extent
                        layer_obj->Set(Nan::New("extent").ToLocalChecked(), Nan::New<v8::Integer>(layer_msg.get_uint32()));
                        break;
                    case 15:
                        version = layer_msg.get_uint32();
                        layer_obj->Set(Nan::New("version").ToLocalChecked(), Nan::New<v8::Integer>(version));
                        break;
                    default:
                        layer_msg.skip();
                        break;
                }
            }
            v8::Local<v8::Array> f_arr = Nan::New<v8::Array>(layer_features.size());
            std::size_t f_idx = 0;
            for (auto feature_msg : layer_features)
            {
                v8::Local<v8::Object> feature_obj = Nan::New<v8::Object>();
                std::pair<protozero::pbf_reader::const_uint32_iterator, protozero::pbf_reader::const_uint32_iterator> geom_itr;
                std::pair<protozero::pbf_reader::const_uint32_iterator, protozero::pbf_reader::const_uint32_iterator> tag_itr;
                bool has_geom = false;
                bool has_geom_type = false;
                bool has_tags = false;
                std::int32_t geom_type_enum = 0;
                while (feature_msg.next())
                {
                    switch (feature_msg.tag())
                    {
                        case 1: // id
                            feature_obj->Set(Nan::New("id").ToLocalChecked(),Nan::New<v8::Number>(feature_msg.get_uint64()));
                            break;
                        case 2: // tags
                            tag_itr = feature_msg.get_packed_uint32();
                            has_tags = true;
                            break;
                        case 3: // geometry type
                            geom_type_enum = feature_msg.get_enum();
                            has_geom_type = true;
                            feature_obj->Set(Nan::New("type").ToLocalChecked(),Nan::New<v8::Integer>(geom_type_enum));
                            break;
                        case 4: // geometry
                            geom_itr = feature_msg.get_packed_uint32();
                            has_geom = true;
                            break;
                        case 5: // raster
                        {
                            auto im_buffer = feature_msg.get_data(); 
                            feature_obj->Set(Nan::New("raster").ToLocalChecked(),
                                             Nan::CopyBuffer(im_buffer.first, im_buffer.second).ToLocalChecked());
                            break;
                        }
                        default:
                            feature_msg.skip();
                            break;
                    }   
                }
                v8::Local<v8::Object> att_obj = Nan::New<v8::Object>();
                if (has_tags)
                {
                    for (auto _i = tag_itr.first; _i != tag_itr.second;)
                    {
                        std::size_t key_name = *(_i++);
                        if (_i == tag_itr.second)
                        {
                            break;
                        }
                        std::size_t key_value = *(_i++);
                        if (key_name < layer_keys.size() &&
                            key_value < layer_values.size())
                        {
                            std::string const& name = layer_keys.at(key_name);
                            mapnik::vector_tile_impl::pbf_attr_value_type val = layer_values.at(key_value);
                            json_value_visitor vv(att_obj, name);
                            mapnik::util::apply_visitor(vv, val);
                        }
                    }
                }
                feature_obj->Set(Nan::New("properties").ToLocalChecked(),att_obj);
                if (has_geom && has_geom_type)
                {
                    if (decode_geometry)
                    {
                        // Decode the geometry first into an int64_t mapnik geometry
                        mapnik::vector_tile_impl::GeometryPBF<std::int64_t> geoms(geom_itr, 0, 0, 1.0, 1.0);
                        mapnik::geometry::geometry<std::int64_t> geom = mapnik::vector_tile_impl::decode_geometry(geoms, geom_type_enum, version);
                        v8::Local<v8::Array> g_arr = geometry_to_array<std::int64_t>(geom);
                        feature_obj->Set(Nan::New("geometry").ToLocalChecked(),g_arr);
                        std::string geom_type = geometry_type_as_string(geom);
                        feature_obj->Set(Nan::New("geometry_type").ToLocalChecked(),Nan::New(geom_type).ToLocalChecked());
                    }
                    else
                    {
                        std::vector<std::uint32_t> geom_vec;
                        for (auto _i = geom_itr.first; _i != geom_itr.second; ++_i)
                        {
                            geom_vec.push_back(*_i);
                        }
                        v8::Local<v8::Array> g_arr = Nan::New<v8::Array>(geom_vec.size());
                        for (std::size_t k = 0; k < geom_vec.size();++k)
                        {
                            g_arr->Set(k,Nan::New<v8::Number>(geom_vec[k]));
                        }
                        feature_obj->Set(Nan::New("geometry").ToLocalChecked(),g_arr);
                    }
                }
                f_arr->Set(f_idx++,feature_obj);
            }
            layer_obj->Set(Nan::New("features").ToLocalChecked(), f_arr);
            arr->Set(l_idx++, layer_obj);
        }
        info.GetReturnValue().Set(arr);
        return;
    }
    catch (std::exception const& ex) 
    {
        Nan::ThrowError(ex.what());
        return;
    }
}

bool layer_to_geojson(protozero::pbf_reader const& layer,
                      std::string & result,
                      unsigned x,
                      unsigned y,
                      unsigned z)
{
    mapnik::vector_tile_impl::tile_datasource_pbf ds(layer, x, y, z);
    mapnik::projection wgs84("+init=epsg:4326",true);
    mapnik::projection merc("+init=epsg:3857",true);
    mapnik::proj_transform prj_trans(merc,wgs84);
    // This mega box ensures we capture all features, including those
    // outside the tile extent. Geometries outside the tile extent are
    // likely when the vtile was created by clipping to a buffered extent
    mapnik::query q(mapnik::box2d<double>(std::numeric_limits<double>::lowest(),
                                          std::numeric_limits<double>::lowest(),
                                          std::numeric_limits<double>::max(),
                                          std::numeric_limits<double>::max()));
    mapnik::layer_descriptor ld = ds.get_descriptor();
    for (auto const& item : ld.get_descriptors())
    {
        q.add_property_name(item.get_name());
    }
    mapnik::featureset_ptr fs = ds.features(q);
    bool first = true;
    if (fs)
    {
        mapnik::feature_ptr feature;
        while ((feature = fs->next()))
        {
            if (first)
            {
                first = false;
            }
            else
            {
                result += "\n,";
            }
            std::string feature_str;
            mapnik::feature_impl feature_new(feature->context(),feature->id());
            feature_new.set_data(feature->get_data());
            unsigned int n_err = 0;
            feature_new.set_geometry(mapnik::geometry::reproject_copy(feature->get_geometry(), prj_trans, n_err));
            if (!mapnik::util::to_geojson(feature_str, feature_new))
            {
                // LCOV_EXCL_START
                throw std::runtime_error("Failed to generate GeoJSON geometry");
                // LCOV_EXCL_STOP
            }
            result += feature_str;
        }
    }
    return !first;
}

/**
 * Get a [GeoJSON](http://geojson.org/) representation of this tile
 *
 * @memberof mapnik.VectorTile
 * @name toGeoJSONSync
 * @instance
 * @returns {string} stringified GeoJSON of all the features in this tile.
 */
NAN_METHOD(VectorTile::toGeoJSONSync)
{
    info.GetReturnValue().Set(_toGeoJSONSync(info));
}

void write_geojson_array(std::string & result,
                         VectorTile * v)
{
    protozero::pbf_reader tile_msg = v->get_tile()->get_reader();
    result += "[";
    bool first = true;
    while (tile_msg.next(3))
    {
        if (first)
        {
            first = false;
        }
        else 
        {
            result += ",";
        }
        auto pair_data = tile_msg.get_data();
        protozero::pbf_reader layer_msg(pair_data);
        protozero::pbf_reader name_msg(pair_data);
        std::string layer_name;
        if (name_msg.next(1))
        {
            layer_name = name_msg.get_string();
        }
        result += "{\"type\":\"FeatureCollection\",";
        result += "\"name\":\"" + layer_name + "\",\"features\":[";
        std::string features;
        bool hit = layer_to_geojson(layer_msg,
                                    features,
                                    v->get_tile()->x(),
                                    v->get_tile()->y(),
                                    v->get_tile()->z());
        if (hit)
        {
            result += features;
        }
        result += "]}";
    }
    result += "]";
}

void write_geojson_all(std::string & result,
                       VectorTile * v)
{
    protozero::pbf_reader tile_msg = v->get_tile()->get_reader();
    result += "{\"type\":\"FeatureCollection\",\"features\":[";
    bool first = true;
    while (tile_msg.next(3))
    {
        protozero::pbf_reader layer_msg(tile_msg.get_message());
        std::string features;
        bool hit = layer_to_geojson(layer_msg,
                                    features,
                                    v->get_tile()->x(),
                                    v->get_tile()->y(),
                                    v->get_tile()->z());
        if (hit)
        {
            if (first)
            {
                first = false;
            }
            else 
            {
                result += ",";
            }
            result += features;
        }
    }
    result += "]}";
}

bool write_geojson_layer_index(std::string & result,
                               std::size_t layer_idx,
                               VectorTile * v)
{
    protozero::pbf_reader layer_msg;
    if (v->get_tile()->layer_reader(layer_idx, layer_msg) &&
        v->get_tile()->get_layers().size() > layer_idx)
    {
        std::string layer_name = v->get_tile()->get_layers()[layer_idx];
        result += "{\"type\":\"FeatureCollection\",";
        result += "\"name\":\"" + layer_name + "\",\"features\":[";
        layer_to_geojson(layer_msg, 
                         result, 
                         v->get_tile()->x(),
                         v->get_tile()->y(),
                         v->get_tile()->z());
        result += "]}";
        return true;
    }
    // LCOV_EXCL_START
    return false;
    // LCOV_EXCL_STOP
}

bool write_geojson_layer_name(std::string & result,
                              std::string const& name,
                              VectorTile * v)
{
    protozero::pbf_reader layer_msg;
    if (v->get_tile()->layer_reader(name, layer_msg))
    {
        result += "{\"type\":\"FeatureCollection\",";
        result += "\"name\":\"" + name + "\",\"features\":[";
        layer_to_geojson(layer_msg, 
                         result, 
                         v->get_tile()->x(),
                         v->get_tile()->y(),
                         v->get_tile()->z());
        result += "]}";
        return true;
    }
    return false;
}

v8::Local<v8::Value> VectorTile::_toGeoJSONSync(Nan::NAN_METHOD_ARGS_TYPE info)
{
    Nan::EscapableHandleScope scope;
    if (info.Length() < 1) 
    {
        Nan::ThrowError("first argument must be either a layer name (string) or layer index (integer)");
        return scope.Escape(Nan::Undefined());
    }
    v8::Local<v8::Value> layer_id = info[0];
    if (! (layer_id->IsString() || layer_id->IsNumber()) )
    {
        Nan::ThrowTypeError("'layer' argument must be either a layer name (string) or layer index (integer)");
        return scope.Escape(Nan::Undefined());
    }

    VectorTile* v = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    std::string result;
    if (layer_id->IsString())
    {
        std::string layer_name = TOSTR(layer_id);
        if (layer_name == "__array__")
        {
            write_geojson_array(result, v);
        }
        else if (layer_name == "__all__")
        {
            write_geojson_all(result, v);
        }
        else
        {
            if (!write_geojson_layer_name(result, layer_name, v))
            {
                std::string error_msg("Layer name '" + layer_name + "' not found");
                Nan::ThrowTypeError(error_msg.c_str());
                return scope.Escape(Nan::Undefined());
            }
        }
    }
    else if (layer_id->IsNumber())
    {
        int layer_idx = layer_id->IntegerValue();
        if (layer_idx < 0)
        {
            Nan::ThrowTypeError("A layer index can not be negative");
            return scope.Escape(Nan::Undefined());
        }
        else if (layer_idx >= static_cast<int>(v->get_tile()->get_layers().size()))
        {
            Nan::ThrowTypeError("Layer index exceeds the number of layers in the vector tile.");
            return scope.Escape(Nan::Undefined());
        }
        if (!write_geojson_layer_index(result, layer_idx, v))
        {
            // LCOV_EXCL_START
            Nan::ThrowTypeError("Layer could not be retrieved (should have not reached here)");
            return scope.Escape(Nan::Undefined());
            // LCOV_EXCL_STOP
        }
    }
    return scope.Escape(Nan::New<v8::String>(result).ToLocalChecked());
}

enum geojson_write_type : std::uint8_t
{
    geojson_write_all = 0,
    geojson_write_array,
    geojson_write_layer_name,
    geojson_write_layer_index
};

struct to_geojson_baton
{
    uv_work_t request;
    VectorTile* v;
    bool error;
    std::string result;
    geojson_write_type type;
    int layer_idx;
    std::string layer_name;
    Nan::Persistent<v8::Function> cb;
};

/**
 * Get a [GeoJSON](http://geojson.org/) representation of this tile
 *
 * @memberof mapnik.VectorTile
 * @name toGeoJSON
 * @instance
 * @param {Function} callback
 * @returns {string} stringified GeoJSON of all the features in this tile.
 */
NAN_METHOD(VectorTile::toGeoJSON)
{
    if ((info.Length() < 1) || !info[info.Length()-1]->IsFunction())
    {
        info.GetReturnValue().Set(_toGeoJSONSync(info));
        return;
    }
    to_geojson_baton *closure = new to_geojson_baton();
    closure->request.data = closure;
    closure->v = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    closure->error = false;
    closure->layer_idx = 0;
    closure->type = geojson_write_all;

    v8::Local<v8::Value> layer_id = info[0];
    if (! (layer_id->IsString() || layer_id->IsNumber()) )
    {
        delete closure;
        Nan::ThrowTypeError("'layer' argument must be either a layer name (string) or layer index (integer)");
        return;
    }

    if (layer_id->IsString())
    {
        std::string layer_name = TOSTR(layer_id);
        if (layer_name == "__array__")
        {
            closure->type = geojson_write_array;
        }
        else if (layer_name == "__all__")
        {
            closure->type = geojson_write_all;
        }
        else
        {
            if (!closure->v->get_tile()->has_layer(layer_name))
            {
                delete closure;
                std::string error_msg("The layer does not contain the name: " + layer_name);
                Nan::ThrowTypeError(error_msg.c_str());
                return;
            }
            closure->layer_name = layer_name;
            closure->type = geojson_write_layer_name;
        }
    }
    else if (layer_id->IsNumber())
    {
        closure->layer_idx = layer_id->IntegerValue();
        if (closure->layer_idx < 0)
        {
            delete closure;
            Nan::ThrowTypeError("A layer index can not be negative");
            return;
        }
        else if (closure->layer_idx >= static_cast<int>(closure->v->get_tile()->get_layers().size()))
        {
            delete closure;
            Nan::ThrowTypeError("Layer index exceeds the number of layers in the vector tile.");
            return;
        }
        closure->type = geojson_write_layer_index;
    }

    v8::Local<v8::Value> callback = info[info.Length()-1];
    closure->cb.Reset(callback.As<v8::Function>());
    uv_queue_work(uv_default_loop(), &closure->request, to_geojson, (uv_after_work_cb)after_to_geojson);
    closure->v->Ref();
    return;
}

void VectorTile::to_geojson(uv_work_t* req)
{
    to_geojson_baton *closure = static_cast<to_geojson_baton *>(req->data);
    try
    {
        switch (closure->type)
        {
            default:
            case geojson_write_all:
                write_geojson_all(closure->result, closure->v);
                break;
            case geojson_write_array:
                write_geojson_array(closure->result, closure->v);
                break;
            case geojson_write_layer_name:
                write_geojson_layer_name(closure->result, closure->layer_name, closure->v);
                break;
            case geojson_write_layer_index:
                write_geojson_layer_index(closure->result, closure->layer_idx, closure->v);
                break;
        }
    }
    catch (std::exception const& ex)
    {
        // There are currently no known ways to trigger this exception in testing. If it was
        // triggered this would likely be a bug in either mapnik or mapnik-vector-tile.
        // LCOV_EXCL_START
        closure->error = true;
        closure->result = ex.what();
        // LCOV_EXCL_STOP
    }
}

void VectorTile::after_to_geojson(uv_work_t* req)
{
    Nan::HandleScope scope;
    to_geojson_baton *closure = static_cast<to_geojson_baton *>(req->data);
    if (closure->error)
    {
        // Because there are no known ways to trigger the exception path in to_geojson
        // there is no easy way to test this path currently
        // LCOV_EXCL_START
        v8::Local<v8::Value> argv[1] = { Nan::Error(closure->result.c_str()) };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 1, argv);
        // LCOV_EXCL_STOP
    }
    else
    {
        v8::Local<v8::Value> argv[2] = { Nan::Null(), Nan::New<v8::String>(closure->result).ToLocalChecked() };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 2, argv);
    }
    closure->v->Unref();
    closure->cb.Reset();
    delete closure;
}

/**
 * Add features to this tile from a GeoJSON string
 *
 * @memberof mapnik.VectorTile
 * @name addGeoJSON
 * @instance
 * @param {string} geojson as a string
 * @param {string} name of the layer to be added
 */
NAN_METHOD(VectorTile::addGeoJSON)
{
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    if (info.Length() < 1 || !info[0]->IsString())
    {
        Nan::ThrowError("first argument must be a GeoJSON string");
        return;
    }
    if (info.Length() < 2 || !info[1]->IsString())
    {
        Nan::ThrowError("second argument must be a layer name (string)");
        return;
    }
    std::string geojson_string = TOSTR(info[0]);
    std::string geojson_name = TOSTR(info[1]);

    v8::Local<v8::Object> options = Nan::New<v8::Object>();
    double area_threshold = 0.1;
    double simplify_distance = 0.0;
    bool strictly_simple = true;
    bool multi_polygon_union = false;
    mapnik::vector_tile_impl::polygon_fill_type fill_type = mapnik::vector_tile_impl::positive_fill;
    bool process_all_rings = false;

    if (info.Length() > 2) 
    {
        // options object
        if (!info[2]->IsObject()) 
        {
            Nan::ThrowError("optional third argument must be an options object");
            return;
        }

        options = info[2]->ToObject();
        if (options->Has(Nan::New("area_threshold").ToLocalChecked())) 
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("area_threshold").ToLocalChecked());
            if (!param_val->IsNumber()) 
            {
                Nan::ThrowError("option 'area_threshold' must be a number");
                return;
            }
            area_threshold = param_val->IntegerValue();
            if (area_threshold < 0.0)
            {
                Nan::ThrowError("option 'area_threshold' can not be negative");
                return;
            }
        }
        if (options->Has(Nan::New("strictly_simple").ToLocalChecked())) 
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("strictly_simple").ToLocalChecked());
            if (!param_val->IsBoolean()) 
            {
                Nan::ThrowError("option 'strictly_simple' must be a boolean");
                return;
            }
            strictly_simple = param_val->BooleanValue();
        }
        if (options->Has(Nan::New("multi_polygon_union").ToLocalChecked()))
        {
            v8::Local<v8::Value> mpu = options->Get(Nan::New("multi_polygon_union").ToLocalChecked());
            if (!mpu->IsBoolean())
            {
                Nan::ThrowTypeError("multi_polygon_union value must be a boolean");
                return;
            }
            multi_polygon_union = mpu->BooleanValue();
        }
        if (options->Has(Nan::New("fill_type").ToLocalChecked())) 
        {
            v8::Local<v8::Value> ft = options->Get(Nan::New("fill_type").ToLocalChecked());
            if (!ft->IsNumber())
            {
                Nan::ThrowTypeError("optional arg 'fill_type' must be a number");
                return;
            }
            fill_type = static_cast<mapnik::vector_tile_impl::polygon_fill_type>(ft->IntegerValue());
            if (fill_type < 0 || fill_type >= mapnik::vector_tile_impl::polygon_fill_type_max)
            {
                Nan::ThrowTypeError("optional arg 'fill_type' out of possible range");
                return;
            }
        }
        if (options->Has(Nan::New("simplify_distance").ToLocalChecked())) 
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("simplify_distance").ToLocalChecked());
            if (!param_val->IsNumber()) 
            {
                Nan::ThrowTypeError("option 'simplify_distance' must be an floating point number");
                return;
            }
            simplify_distance = param_val->NumberValue();
            if (simplify_distance < 0.0)
            {
                Nan::ThrowTypeError("option 'simplify_distance' must be a positive number");
                return;
            }
        }
        if (options->Has(Nan::New("process_all_rings").ToLocalChecked())) 
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("process_all_rings").ToLocalChecked());
            if (!param_val->IsBoolean()) 
            {
                Nan::ThrowTypeError("option 'process_all_rings' must be a boolean");
                return;
            }
            process_all_rings = param_val->BooleanValue();
        }
    }

    try
    {
        // create map object
        mapnik::Map map(d->tile_size(),d->tile_size(),"+init=epsg:3857");
        mapnik::parameters p;
        p["type"]="geojson";
        p["inline"]=geojson_string;
        mapnik::layer lyr(geojson_name,"+init=epsg:4326");
        lyr.set_datasource(mapnik::datasource_cache::instance().create(p));
        map.add_layer(lyr);
        
        mapnik::vector_tile_impl::processor ren(map);
        ren.set_area_threshold(area_threshold);
        ren.set_strictly_simple(strictly_simple);
        ren.set_simplify_distance(simplify_distance);
        ren.set_multi_polygon_union(multi_polygon_union);
        ren.set_fill_type(fill_type);
        ren.set_process_all_rings(process_all_rings);
        ren.update_tile(*d->get_tile());
        info.GetReturnValue().Set(Nan::True());
    }
    catch (std::exception const& ex)
    {
        Nan::ThrowError(ex.what());
        return;
    }
}

/**
 * Add an Image as a tile layer
 *
 * @memberof mapnik.VectorTile
 * @name addImageSync
 * @instance
 * @param {mapnik.Image} image
 * @param {string} name of the layer to be added
 */
NAN_METHOD(VectorTile::addImageSync)
{
    info.GetReturnValue().Set(_addImageSync(info));
}

v8::Local<v8::Value> VectorTile::_addImageSync(Nan::NAN_METHOD_ARGS_TYPE info)
{
    Nan::EscapableHandleScope scope;
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    if (info.Length() < 1 || !info[0]->IsObject())
    {
        Nan::ThrowError("first argument must be an Image object");
        return scope.Escape(Nan::Undefined());
    }
    if (info.Length() < 2 || !info[1]->IsString())
    {
        Nan::ThrowError("second argument must be a layer name (string)");
        return scope.Escape(Nan::Undefined());
    }
    std::string layer_name = TOSTR(info[1]);
    v8::Local<v8::Object> obj = info[0]->ToObject();
    if (obj->IsNull() || 
        obj->IsUndefined() || 
        !Nan::New(Image::constructor)->HasInstance(obj))
    {
        Nan::ThrowError("first argument must be an Image object");
        return scope.Escape(Nan::Undefined());
    }
    Image *im = Nan::ObjectWrap::Unwrap<Image>(obj);
    if (im->get()->width() <= 0 || im->get()->height() <= 0)
    {
        Nan::ThrowError("Image width and height must be greater then zero");
        return scope.Escape(Nan::Undefined());
    }

    std::string image_format = "webp";
    mapnik::scaling_method_e scaling_method = mapnik::SCALING_BILINEAR;
    
    if (info.Length() > 2) 
    {
        // options object
        if (!info[2]->IsObject()) 
        {
            Nan::ThrowError("optional third argument must be an options object");
            return scope.Escape(Nan::Undefined());
        }

        v8::Local<v8::Object> options = info[2]->ToObject();
        if (options->Has(Nan::New("image_scaling").ToLocalChecked())) 
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("image_scaling").ToLocalChecked());
            if (!param_val->IsString()) 
            {
                Nan::ThrowTypeError("option 'image_scaling' must be a string");
                return scope.Escape(Nan::Undefined());
            }
            std::string image_scaling = TOSTR(param_val);
            boost::optional<mapnik::scaling_method_e> method = mapnik::scaling_method_from_string(image_scaling);
            if (!method) 
            {
                Nan::ThrowTypeError("option 'image_scaling' must be a string and a valid scaling method (e.g 'bilinear')");
                return scope.Escape(Nan::Undefined());
            }
            scaling_method = *method;
        }

        if (options->Has(Nan::New("image_format").ToLocalChecked())) 
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("image_format").ToLocalChecked());
            if (!param_val->IsString()) 
            {
                Nan::ThrowTypeError("option 'image_format' must be a string");
                return scope.Escape(Nan::Undefined());
            }
            image_format = TOSTR(param_val);
        }
    }
    mapnik::image_any im_copy = *im->get();
    std::shared_ptr<mapnik::memory_datasource> ds = std::make_shared<mapnik::memory_datasource>(mapnik::parameters());
    mapnik::raster_ptr ras = std::make_shared<mapnik::raster>(d->get_tile()->extent(), im_copy, 1.0);
    mapnik::context_ptr ctx = std::make_shared<mapnik::context_type>();
    mapnik::feature_ptr feature(mapnik::feature_factory::create(ctx,1));
    feature->set_raster(ras);
    ds->push(feature);
    ds->envelope(); // can be removed later, currently doesn't work with out this.
    ds->set_envelope(d->get_tile()->extent());
    try
    {
        // create map object
        mapnik::Map map(d->tile_size(),d->tile_size(),"+init=epsg:3857");
        mapnik::layer lyr(layer_name,"+init=epsg:3857");
        lyr.set_datasource(ds);
        map.add_layer(lyr);
        
        mapnik::vector_tile_impl::processor ren(map);
        ren.set_scaling_method(scaling_method);
        ren.set_image_format(image_format);
        ren.update_tile(*d->get_tile());
        info.GetReturnValue().Set(Nan::True());
    }
    catch (std::exception const& ex)
    {
        Nan::ThrowError(ex.what());
        return scope.Escape(Nan::Undefined());
    }
    return scope.Escape(Nan::Undefined());
}

typedef struct
{
    uv_work_t request;
    VectorTile* d;
    Image* im;
    std::string layer_name;
    std::string image_format;
    mapnik::scaling_method_e scaling_method;
    bool error;
    std::string error_name;
    Nan::Persistent<v8::Function> cb;
} vector_tile_add_image_baton_t;

/**
 * Add an Image as a tile layer
 *
 * @memberof mapnik.VectorTile
 * @name addImageSync
 * @instance
 * @param {mapnik.Image} image
 * @param {string} name of the layer to be added
 */
NAN_METHOD(VectorTile::addImage)
{
    // If last param is not a function assume sync
    if (info.Length() < 2)
    {
        Nan::ThrowError("addImage requires at least two parameters: an Image and a layer name");
        return;
    }
    v8::Local<v8::Value> callback = info[info.Length() - 1];
    if (!info[info.Length() - 1]->IsFunction())
    {
        info.GetReturnValue().Set(_addImageSync(info));
        return;
    }
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.This());
    if (!info[0]->IsObject())
    {
        Nan::ThrowError("first argument must be an Image object");
        return;
    }
    if (!info[1]->IsString())
    {
        Nan::ThrowError("second argument must be a layer name (string)");
        return;
    }
    std::string layer_name = TOSTR(info[1]);
    v8::Local<v8::Object> obj = info[0]->ToObject();
    if (obj->IsNull() || 
        obj->IsUndefined() || 
        !Nan::New(Image::constructor)->HasInstance(obj))
    {
        Nan::ThrowError("first argument must be an Image object");
        return;
    }
    Image *im = Nan::ObjectWrap::Unwrap<Image>(obj);
    if (im->get()->width() <= 0 || im->get()->height() <= 0)
    {
        Nan::ThrowError("Image width and height must be greater then zero");
        return;
    }

    std::string image_format = "webp";
    mapnik::scaling_method_e scaling_method = mapnik::SCALING_BILINEAR;
    
    if (info.Length() > 3) 
    {
        // options object
        if (!info[2]->IsObject()) 
        {
            Nan::ThrowError("optional third argument must be an options object");
            return;
        }

        v8::Local<v8::Object> options = info[2]->ToObject();
        if (options->Has(Nan::New("image_scaling").ToLocalChecked())) 
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("image_scaling").ToLocalChecked());
            if (!param_val->IsString()) 
            {
                Nan::ThrowTypeError("option 'image_scaling' must be a string");
                return;
            }
            std::string image_scaling = TOSTR(param_val);
            boost::optional<mapnik::scaling_method_e> method = mapnik::scaling_method_from_string(image_scaling);
            if (!method) 
            {
                Nan::ThrowTypeError("option 'image_scaling' must be a string and a valid scaling method (e.g 'bilinear')");
                return;
            }
            scaling_method = *method;
        }

        if (options->Has(Nan::New("image_format").ToLocalChecked())) 
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("image_format").ToLocalChecked());
            if (!param_val->IsString()) 
            {
                Nan::ThrowTypeError("option 'image_format' must be a string");
                return;
            }
            image_format = TOSTR(param_val);
        }
    }
    vector_tile_add_image_baton_t *closure = new vector_tile_add_image_baton_t();
    closure->request.data = closure;
    closure->d = d;
    closure->im = im;
    closure->scaling_method = scaling_method;
    closure->image_format = image_format;
    closure->layer_name = layer_name;
    closure->error = false;
    closure->cb.Reset(callback.As<v8::Function>());
    uv_queue_work(uv_default_loop(), &closure->request, EIO_AddImage, (uv_after_work_cb)EIO_AfterAddImage);
    d->Ref();
    im->_ref();
    return;
}

void VectorTile::EIO_AddImage(uv_work_t* req)
{
    vector_tile_add_image_baton_t *closure = static_cast<vector_tile_add_image_baton_t *>(req->data);

    try
    {
        mapnik::image_any im_copy = *closure->im->get();
        std::shared_ptr<mapnik::memory_datasource> ds = std::make_shared<mapnik::memory_datasource>(mapnik::parameters());
        mapnik::raster_ptr ras = std::make_shared<mapnik::raster>(closure->d->get_tile()->extent(), im_copy, 1.0);
        mapnik::context_ptr ctx = std::make_shared<mapnik::context_type>();
        mapnik::feature_ptr feature(mapnik::feature_factory::create(ctx,1));
        feature->set_raster(ras);
        ds->push(feature);
        ds->envelope(); // can be removed later, currently doesn't work with out this.
        ds->set_envelope(closure->d->get_tile()->extent());
        // create map object
        mapnik::Map map(closure->d->tile_size(),closure->d->tile_size(),"+init=epsg:3857");
        mapnik::layer lyr(closure->layer_name,"+init=epsg:3857");
        lyr.set_datasource(ds);
        map.add_layer(lyr);
        
        mapnik::vector_tile_impl::processor ren(map);
        ren.set_scaling_method(closure->scaling_method);
        ren.set_image_format(closure->image_format);
        ren.update_tile(*closure->d->get_tile());
    }
    catch (std::exception const& ex)
    {
        closure->error = true;
        closure->error_name = ex.what();
    }
}

void VectorTile::EIO_AfterAddImage(uv_work_t* req)
{
    Nan::HandleScope scope;
    vector_tile_add_image_baton_t *closure = static_cast<vector_tile_add_image_baton_t *>(req->data);
    if (closure->error)
    {
        v8::Local<v8::Value> argv[1] = { Nan::Error(closure->error_name.c_str()) };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 1, argv);
    }
    else
    {
        v8::Local<v8::Value> argv[1] = { Nan::Null() };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 1, argv);
    }

    closure->d->Unref();
    closure->im->_unref();
    closure->cb.Reset();
    delete closure;
}

/**
 * Add raw image buffer as a new tile layer
 *
 * @memberof mapnik.VectorTile
 * @name addImageBufferSync
 * @instance
 * @param {Buffer} raw data
 * @param {string} name of the layer to be added
 */
NAN_METHOD(VectorTile::addImageBufferSync)
{
    info.GetReturnValue().Set(_addImageBufferSync(info));
}

v8::Local<v8::Value> VectorTile::_addImageBufferSync(Nan::NAN_METHOD_ARGS_TYPE info)
{
    Nan::EscapableHandleScope scope;
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    if (info.Length() < 1 || !info[0]->IsObject())
    {
        Nan::ThrowTypeError("first argument must be a buffer object");
        return scope.Escape(Nan::Undefined());
    }
    if (info.Length() < 2 || !info[1]->IsString())
    {
        Nan::ThrowError("second argument must be a layer name (string)");
        return scope.Escape(Nan::Undefined());
    }
    std::string layer_name = TOSTR(info[1]);
    v8::Local<v8::Object> obj = info[0]->ToObject();
    if (obj->IsNull() || obj->IsUndefined() || !node::Buffer::HasInstance(obj))
    {
        Nan::ThrowTypeError("first arg must be a buffer object");
        return scope.Escape(Nan::Undefined());
    }
    std::size_t buffer_size = node::Buffer::Length(obj);
    if (buffer_size <= 0)
    {
        Nan::ThrowError("cannot accept empty buffer as protobuf");
        return scope.Escape(Nan::Undefined());
    }
    try
    {
        add_image_buffer_as_tile_layer(*d->get_tile(), layer_name, node::Buffer::Data(obj), buffer_size); 
    }
    catch (std::exception const& ex)
    {
        Nan::ThrowError(ex.what());
        return scope.Escape(Nan::Undefined());
    }
    return scope.Escape(Nan::Undefined());
}

typedef struct
{
    uv_work_t request;
    VectorTile* d;
    const char *data;
    size_t dataLength;
    std::string layer_name;
    bool error;
    std::string error_name;
    Nan::Persistent<v8::Function> cb;
    Nan::Persistent<v8::Object> buffer;
} vector_tile_addimagebuffer_baton_t;


/**
 * Add an encoded image buffer as a layer
 *
 * @memberof mapnik.VectorTile
 * @name addImageBuffer
 * @instance
 * @param {Buffer} raw data
 * @param {string} name of the layer to be added
 */
NAN_METHOD(VectorTile::addImageBuffer)
{
    if (info.Length() < 3)
    {
        info.GetReturnValue().Set(_addImageBufferSync(info));
        return;
    }

    // ensure callback is a function
    v8::Local<v8::Value> callback = info[info.Length() - 1];
    if (!info[info.Length() - 1]->IsFunction())
    {
        Nan::ThrowTypeError("last argument must be a callback function");
        return;
    }

    if (info.Length() < 1 || !info[0]->IsObject())
    {
        Nan::ThrowTypeError("first argument must be a buffer object");
        return;
    }
    if (info.Length() < 2 || !info[1]->IsString())
    {
        Nan::ThrowError("second argument must be a layer name (string)");
        return;
    }
    std::string layer_name = TOSTR(info[1]);
    v8::Local<v8::Object> obj = info[0]->ToObject();
    if (obj->IsNull() || obj->IsUndefined() || !node::Buffer::HasInstance(obj))
    {
        Nan::ThrowTypeError("first arg must be a buffer object");
        return;
    }

    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());

    vector_tile_addimagebuffer_baton_t *closure = new vector_tile_addimagebuffer_baton_t();
    closure->request.data = closure;
    closure->d = d;
    closure->layer_name = layer_name;
    closure->error = false;
    closure->cb.Reset(callback.As<v8::Function>());
    closure->buffer.Reset(obj.As<v8::Object>());
    closure->data = node::Buffer::Data(obj);
    closure->dataLength = node::Buffer::Length(obj);
    uv_queue_work(uv_default_loop(), &closure->request, EIO_AddImageBuffer, (uv_after_work_cb)EIO_AfterAddImageBuffer);
    d->Ref();
    return;
}

void VectorTile::EIO_AddImageBuffer(uv_work_t* req)
{
    vector_tile_addimagebuffer_baton_t *closure = static_cast<vector_tile_addimagebuffer_baton_t *>(req->data);

    try
    {
        add_image_buffer_as_tile_layer(*closure->d->get_tile(), closure->layer_name, closure->data, closure->dataLength); 
    }
    catch (std::exception const& ex)
    {
        closure->error = true;
        closure->error_name = ex.what();
    }
}

void VectorTile::EIO_AfterAddImageBuffer(uv_work_t* req)
{
    Nan::HandleScope scope;
    vector_tile_addimagebuffer_baton_t *closure = static_cast<vector_tile_addimagebuffer_baton_t *>(req->data);
    if (closure->error)
    {
        v8::Local<v8::Value> argv[1] = { Nan::Error(closure->error_name.c_str()) };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 1, argv);
    }
    else
    {
        v8::Local<v8::Value> argv[1] = { Nan::Null() };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 1, argv);
    }

    closure->d->Unref();
    closure->cb.Reset();
    closure->buffer.Reset();
    delete closure;
}

/**
 * Add raw data to this tile as a Buffer
 *
 * @memberof mapnik.VectorTile
 * @name addDataSync
 * @instance
 * @param {Buffer} raw data
 */
NAN_METHOD(VectorTile::addDataSync)
{
    info.GetReturnValue().Set(_addDataSync(info));
}

v8::Local<v8::Value> VectorTile::_addDataSync(Nan::NAN_METHOD_ARGS_TYPE info)
{
    Nan::EscapableHandleScope scope;
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    if (info.Length() < 1 || !info[0]->IsObject())
    {
        Nan::ThrowTypeError("first argument must be a buffer object");
        return scope.Escape(Nan::Undefined());
    }
    v8::Local<v8::Object> obj = info[0]->ToObject();
    if (obj->IsNull() || obj->IsUndefined() || !node::Buffer::HasInstance(obj))
    {
        Nan::ThrowTypeError("first arg must be a buffer object");
        return scope.Escape(Nan::Undefined());
    }
    std::size_t buffer_size = node::Buffer::Length(obj);
    if (buffer_size <= 0)
    {
        Nan::ThrowError("cannot accept empty buffer as protobuf");
        return scope.Escape(Nan::Undefined());
    }
    try
    {
        merge_from_compressed_buffer(*d->get_tile(), node::Buffer::Data(obj), buffer_size); 
    }
    catch (std::exception const& ex)
    {
        Nan::ThrowError(ex.what());
        return scope.Escape(Nan::Undefined());
    }
    return scope.Escape(Nan::Undefined());
}

typedef struct
{
    uv_work_t request;
    VectorTile* d;
    const char *data;
    size_t dataLength;
    bool error;
    std::string error_name;
    Nan::Persistent<v8::Function> cb;
    Nan::Persistent<v8::Object> buffer;
} vector_tile_adddata_baton_t;


/**
 * Add new vector tile data to an existing vector tile
 *
 * @memberof mapnik.VectorTile
 * @name addData
 * @instance
 * @param {Buffer} raw data
 */
NAN_METHOD(VectorTile::addData)
{
    if (info.Length() == 1)
    {
        info.GetReturnValue().Set(_addDataSync(info));
        return;
    }

    // ensure callback is a function
    v8::Local<v8::Value> callback = info[info.Length() - 1];
    if (!info[info.Length() - 1]->IsFunction())
    {
        Nan::ThrowTypeError("last argument must be a callback function");
        return;
    }

    if (info.Length() < 1 || !info[0]->IsObject())
    {
        Nan::ThrowTypeError("first argument must be a buffer object");
        return;
    }
    v8::Local<v8::Object> obj = info[0]->ToObject();
    if (obj->IsNull() || obj->IsUndefined() || !node::Buffer::HasInstance(obj))
    {
        Nan::ThrowTypeError("first arg must be a buffer object");
        return;
    }

    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());

    vector_tile_adddata_baton_t *closure = new vector_tile_adddata_baton_t();
    closure->request.data = closure;
    closure->d = d;
    closure->error = false;
    closure->cb.Reset(callback.As<v8::Function>());
    closure->buffer.Reset(obj.As<v8::Object>());
    closure->data = node::Buffer::Data(obj);
    closure->dataLength = node::Buffer::Length(obj);
    uv_queue_work(uv_default_loop(), &closure->request, EIO_AddData, (uv_after_work_cb)EIO_AfterAddData);
    d->Ref();
    return;
}

void VectorTile::EIO_AddData(uv_work_t* req)
{
    vector_tile_adddata_baton_t *closure = static_cast<vector_tile_adddata_baton_t *>(req->data);

    if (closure->dataLength <= 0)
    {
        closure->error = true;
        closure->error_name = "cannot accept empty buffer as protobuf";
        return;
    }
    try
    {
        merge_from_compressed_buffer(*closure->d->get_tile(), closure->data, closure->dataLength); 
    }
    catch (std::exception const& ex)
    {
        closure->error = true;
        closure->error_name = ex.what();
    }
}

void VectorTile::EIO_AfterAddData(uv_work_t* req)
{
    Nan::HandleScope scope;
    vector_tile_adddata_baton_t *closure = static_cast<vector_tile_adddata_baton_t *>(req->data);
    if (closure->error)
    {
        v8::Local<v8::Value> argv[1] = { Nan::Error(closure->error_name.c_str()) };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 1, argv);
    }
    else
    {
        v8::Local<v8::Value> argv[1] = { Nan::Null() };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 1, argv);
    }

    closure->d->Unref();
    closure->cb.Reset();
    closure->buffer.Reset();
    delete closure;
}

/**
 * Replace the data in this vector tile with new raw data
 *
 * @memberof mapnik.VectorTile
 * @name setDataSync
 * @instance
 * @param {Buffer} raw data
 */
NAN_METHOD(VectorTile::setDataSync)
{
    info.GetReturnValue().Set(_setDataSync(info));
}

v8::Local<v8::Value> VectorTile::_setDataSync(Nan::NAN_METHOD_ARGS_TYPE info)
{
    Nan::EscapableHandleScope scope;
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    if (info.Length() < 1 || !info[0]->IsObject())
    {
        Nan::ThrowTypeError("first argument must be a buffer object");
        return scope.Escape(Nan::Undefined());
    }
    v8::Local<v8::Object> obj = info[0]->ToObject();
    if (obj->IsNull() || obj->IsUndefined() || !node::Buffer::HasInstance(obj))
    {
        Nan::ThrowTypeError("first arg must be a buffer object");
        return scope.Escape(Nan::Undefined());
    }
    std::size_t buffer_size = node::Buffer::Length(obj);
    if (buffer_size <= 0)
    {
        Nan::ThrowError("cannot accept empty buffer as protobuf");
        return scope.Escape(Nan::Undefined());
    }
    try
    {
        d->clear();
        merge_from_compressed_buffer(*d->get_tile(), node::Buffer::Data(obj), buffer_size); 
    }
    catch (std::exception const& ex)
    {
        Nan::ThrowError(ex.what());
        return scope.Escape(Nan::Undefined());
    }
    return scope.Escape(Nan::Undefined());
}

typedef struct
{
    uv_work_t request;
    VectorTile* d;
    const char *data;
    size_t dataLength;
    bool error;
    std::string error_name;
    Nan::Persistent<v8::Function> cb;
    Nan::Persistent<v8::Object> buffer;
} vector_tile_setdata_baton_t;


/**
 * Replace the data in this vector tile with new raw data
 *
 * @memberof mapnik.VectorTile
 * @name setData
 * @instance
 * @param {Buffer} raw data
 */
NAN_METHOD(VectorTile::setData)
{
    if (info.Length() == 1)
    {
        info.GetReturnValue().Set(_setDataSync(info));
        return;
    }

    // ensure callback is a function
    v8::Local<v8::Value> callback = info[info.Length() - 1];
    if (!info[info.Length() - 1]->IsFunction())
    {
        Nan::ThrowTypeError("last argument must be a callback function");
        return;
    }

    if (info.Length() < 1 || !info[0]->IsObject())
    {
        Nan::ThrowTypeError("first argument must be a buffer object");
        return;
    }
    v8::Local<v8::Object> obj = info[0]->ToObject();
    if (obj->IsNull() || obj->IsUndefined() || !node::Buffer::HasInstance(obj))
    {
        Nan::ThrowTypeError("first arg must be a buffer object");
        return;
    }

    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());

    vector_tile_setdata_baton_t *closure = new vector_tile_setdata_baton_t();
    closure->request.data = closure;
    closure->d = d;
    closure->error = false;
    closure->cb.Reset(callback.As<v8::Function>());
    closure->buffer.Reset(obj.As<v8::Object>());
    closure->data = node::Buffer::Data(obj);
    closure->dataLength = node::Buffer::Length(obj);
    uv_queue_work(uv_default_loop(), &closure->request, EIO_SetData, (uv_after_work_cb)EIO_AfterSetData);
    d->Ref();
    return;
}

void VectorTile::EIO_SetData(uv_work_t* req)
{
    vector_tile_setdata_baton_t *closure = static_cast<vector_tile_setdata_baton_t *>(req->data);
    
    if (closure->dataLength <= 0)
    {
        closure->error = true;
        closure->error_name = "cannot accept empty buffer as protobuf";
        return;
    }

    try
    {
        closure->d->clear();
        merge_from_compressed_buffer(*closure->d->get_tile(), closure->data, closure->dataLength); 
    }
    catch (std::exception const& ex)
    {
        closure->error = true;
        closure->error_name = ex.what();
    }
}

void VectorTile::EIO_AfterSetData(uv_work_t* req)
{
    Nan::HandleScope scope;
    vector_tile_setdata_baton_t *closure = static_cast<vector_tile_setdata_baton_t *>(req->data);
    if (closure->error)
    {
        v8::Local<v8::Value> argv[1] = { Nan::Error(closure->error_name.c_str()) };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 1, argv);
    }
    else
    {
        v8::Local<v8::Value> argv[1] = { Nan::Null() };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 1, argv);
    }

    closure->d->Unref();
    closure->cb.Reset();
    closure->buffer.Reset();
    delete closure;
}

/**
 * Get the data in this vector tile as a buffer
 *
 * @memberof mapnik.VectorTile
 * @name getDataSync
 * @instance
 * @returns {Buffer} raw data
 */

NAN_METHOD(VectorTile::getDataSync)
{
    info.GetReturnValue().Set(_getDataSync(info));
}

v8::Local<v8::Value> VectorTile::_getDataSync(Nan::NAN_METHOD_ARGS_TYPE info)
{
    Nan::EscapableHandleScope scope;
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());

    bool compress = false;
    int level = Z_DEFAULT_COMPRESSION;
    int strategy = Z_DEFAULT_STRATEGY;

    v8::Local<v8::Object> options = Nan::New<v8::Object>();

    if (info.Length() > 0)
    {
        if (!info[0]->IsObject())
        {
            Nan::ThrowTypeError("first arg must be a options object");
            return scope.Escape(Nan::Undefined());
        }

        options = info[0]->ToObject();

        if (options->Has(Nan::New<v8::String>("compression").ToLocalChecked()))
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("compression").ToLocalChecked());
            if (!param_val->IsString())
            {
                Nan::ThrowTypeError("option 'compression' must be a string, either 'gzip', or 'none' (default)");
                return scope.Escape(Nan::Undefined());
            }
            compress = std::string("gzip") == (TOSTR(param_val->ToString()));
        }

        if (options->Has(Nan::New<v8::String>("level").ToLocalChecked()))
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("level").ToLocalChecked());
            if (!param_val->IsNumber())
            {
                Nan::ThrowTypeError("option 'level' must be an integer between 0 (no compression) and 9 (best compression) inclusive");
                return scope.Escape(Nan::Undefined());
            }
            level = param_val->IntegerValue();
            if (level < 0 || level > 9)
            {
                Nan::ThrowTypeError("option 'level' must be an integer between 0 (no compression) and 9 (best compression) inclusive");
                return scope.Escape(Nan::Undefined());
            }
        }
        if (options->Has(Nan::New<v8::String>("strategy").ToLocalChecked()))
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("strategy").ToLocalChecked());
            if (!param_val->IsString())
            {
                Nan::ThrowTypeError("option 'strategy' must be one of the following strings: FILTERED, HUFFMAN_ONLY, RLE, FIXED, DEFAULT");
                return scope.Escape(Nan::Undefined());
            }
            else if (std::string("FILTERED") == TOSTR(param_val->ToString()))
            {
                strategy = Z_FILTERED;
            }
            else if (std::string("HUFFMAN_ONLY") == TOSTR(param_val->ToString()))
            {
                strategy = Z_HUFFMAN_ONLY;
            }
            else if (std::string("RLE") == TOSTR(param_val->ToString()))
            {
                strategy = Z_RLE;
            }
            else if (std::string("FIXED") == TOSTR(param_val->ToString()))
            {
                strategy = Z_FIXED;
            }
            else if (std::string("DEFAULT") == TOSTR(param_val->ToString()))
            {
                strategy = Z_DEFAULT_STRATEGY;
            }
            else
            {
                Nan::ThrowTypeError("option 'strategy' must be one of the following strings: FILTERED, HUFFMAN_ONLY, RLE, FIXED, DEFAULT");
                return scope.Escape(Nan::Undefined());
            }
        }
    }

    try
    {
        std::size_t raw_size = d->tile_->size();
        if (raw_size <= 0)
        {
            return scope.Escape(Nan::NewBuffer(0).ToLocalChecked());
        }
        else
        {
            if (raw_size >= node::Buffer::kMaxLength) {
                // This is a valid test path, but I am excluding it from test coverage due to the
                // requirement of loading a very large object in memory in order to test it.
                // LCOV_EXCL_START
                std::ostringstream s;
                s << "Data is too large to convert to a node::Buffer ";
                s << "(" << raw_size << " raw bytes >= node::Buffer::kMaxLength)";
                Nan::ThrowTypeError(s.str().c_str());
                return scope.Escape(Nan::Undefined());
                // LCOV_EXCL_STOP
            }
            if (!compress)
            {
                return scope.Escape(Nan::CopyBuffer((char*)d->tile_->data(),raw_size).ToLocalChecked());
            }
            else
            {
                std::string compressed;
                mapnik::vector_tile_impl::zlib_compress(d->tile_->data(), raw_size, compressed, true, level, strategy);
                return scope.Escape(Nan::CopyBuffer((char*)compressed.data(),compressed.size()).ToLocalChecked());
            }
        }
    } 
    catch (std::exception const& ex) 
    {
        // As all exception throwing paths are not easily testable or no way can be
        // found to test with repeatability this exception path is not included
        // in test coverage.
        // LCOV_EXCL_START
        Nan::ThrowTypeError(ex.what());
        return scope.Escape(Nan::Undefined());
        // LCOV_EXCL_STOP
    }
    return scope.Escape(Nan::Undefined());
}

typedef struct
{
    uv_work_t request;
    VectorTile* d;
    bool error;
    std::string data;
    bool compress;
    int level;
    int strategy;
    std::string error_name;
    Nan::Persistent<v8::Function> cb;
} vector_tile_get_data_baton_t;


NAN_METHOD(VectorTile::getData)
{
    if (info.Length() == 0 || !info[info.Length()-1]->IsFunction())
    {
        info.GetReturnValue().Set(_getDataSync(info));
        return;
    }

    v8::Local<v8::Value> callback = info[info.Length()-1];
    bool compress = false;
    int level = Z_DEFAULT_COMPRESSION;
    int strategy = Z_DEFAULT_STRATEGY;

    v8::Local<v8::Object> options = Nan::New<v8::Object>();

    if (info.Length() > 1)
    {
        if (!info[0]->IsObject())
        {
            Nan::ThrowTypeError("first arg must be a options object");
            return;
        }

        options = info[0]->ToObject();

        if (options->Has(Nan::New("compression").ToLocalChecked()))
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("compression").ToLocalChecked());
            if (!param_val->IsString())
            {
                Nan::ThrowTypeError("option 'compression' must be a string, either 'gzip', or 'none' (default)");
                return;
            }
            compress = std::string("gzip") == (TOSTR(param_val->ToString()));
        }

        if (options->Has(Nan::New("level").ToLocalChecked()))
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("level").ToLocalChecked());
            if (!param_val->IsNumber())
            {
                Nan::ThrowTypeError("option 'level' must be an integer between 0 (no compression) and 9 (best compression) inclusive");
                return;
            }
            level = param_val->IntegerValue();
            if (level < 0 || level > 9)
            {
                Nan::ThrowTypeError("option 'level' must be an integer between 0 (no compression) and 9 (best compression) inclusive");
                return;
            }
        }
        if (options->Has(Nan::New("strategy").ToLocalChecked()))
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("strategy").ToLocalChecked());
            if (!param_val->IsString())
            {
                Nan::ThrowTypeError("option 'strategy' must be one of the following strings: FILTERED, HUFFMAN_ONLY, RLE, FIXED, DEFAULT");
                return;
            }
            else if (std::string("FILTERED") == TOSTR(param_val->ToString()))
            {
                strategy = Z_FILTERED;
            }
            else if (std::string("HUFFMAN_ONLY") == TOSTR(param_val->ToString()))
            {
                strategy = Z_HUFFMAN_ONLY;
            }
            else if (std::string("RLE") == TOSTR(param_val->ToString()))
            {
                strategy = Z_RLE;
            }
            else if (std::string("FIXED") == TOSTR(param_val->ToString()))
            {
                strategy = Z_FIXED;
            }
            else if (std::string("DEFAULT") == TOSTR(param_val->ToString()))
            {
                strategy = Z_DEFAULT_STRATEGY;
            }
            else
            {
                Nan::ThrowTypeError("option 'strategy' must be one of the following strings: FILTERED, HUFFMAN_ONLY, RLE, FIXED, DEFAULT");
                return;
            }
        }
    }

    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    vector_tile_get_data_baton_t *closure = new vector_tile_get_data_baton_t();
    closure->request.data = closure;
    closure->d = d;
    closure->compress = compress;
    closure->level = level;
    closure->strategy = strategy;
    closure->error = false;
    closure->cb.Reset(callback.As<v8::Function>());
    uv_queue_work(uv_default_loop(), &closure->request, get_data, (uv_after_work_cb)after_get_data);
    d->Ref();
    return;
}

void VectorTile::get_data(uv_work_t* req)
{
    vector_tile_get_data_baton_t *closure = static_cast<vector_tile_get_data_baton_t *>(req->data);
    try
    {
        // compress if requested
        if (closure->compress)
        {
            mapnik::vector_tile_impl::zlib_compress(closure->d->tile_->data(), closure->d->tile_->size(), closure->data, true, closure->level, closure->strategy);
        }
    }
    catch (std::exception const& ex)
    {
        // As all exception throwing paths are not easily testable or no way can be
        // found to test with repeatability this exception path is not included
        // in test coverage.
        // LCOV_EXCL_START
        closure->error = true;
        closure->error_name = ex.what();
        // LCOV_EXCL_STOP
    }
}

void VectorTile::after_get_data(uv_work_t* req)
{
    Nan::HandleScope scope;
    vector_tile_get_data_baton_t *closure = static_cast<vector_tile_get_data_baton_t *>(req->data);
    if (closure->error) 
    {
        // As all exception throwing paths are not easily testable or no way can be
        // found to test with repeatability this exception path is not included
        // in test coverage.
        // LCOV_EXCL_START
        v8::Local<v8::Value> argv[1] = { Nan::Error(closure->error_name.c_str()) };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 1, argv);
        // LCOV_EXCL_STOP
    }
    else if (!closure->data.empty())
    {
        v8::Local<v8::Value> argv[2] = { Nan::Null(), Nan::CopyBuffer((char*)closure->data.data(),closure->data.size()).ToLocalChecked() };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 2, argv);
    }
    else
    {
        std::size_t raw_size = closure->d->tile_->size();
        if (raw_size <= 0)
        {
            v8::Local<v8::Value> argv[2] = { Nan::Null(), Nan::NewBuffer(0).ToLocalChecked() };
            Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 2, argv);
        }
        else if (raw_size >= node::Buffer::kMaxLength)
        {
            // This is a valid test path, but I am excluding it from test coverage due to the
            // requirement of loading a very large object in memory in order to test it.
            // LCOV_EXCL_START
            std::ostringstream s;
            s << "Data is too large to convert to a node::Buffer ";
            s << "(" << raw_size << " raw bytes >= node::Buffer::kMaxLength)";
            v8::Local<v8::Value> argv[1] = { Nan::Error(s.str().c_str()) };
            Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 1, argv);
            // LCOV_EXCL_STOP
        }
        else
        {
            v8::Local<v8::Value> argv[2] = { Nan::Null(), Nan::CopyBuffer((char*)closure->d->tile_->data(),raw_size).ToLocalChecked() };
            Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 2, argv);
        }
    }

    closure->d->Unref();
    closure->cb.Reset();
    delete closure;
}

using surface_type = mapnik::util::variant<Image *
  , CairoSurface *
#if defined(GRID_RENDERER)
  , Grid *
#endif
  >;

struct deref_visitor
{
    template <typename SurfaceType>
    void operator() (SurfaceType * surface)
    {
        if (surface != nullptr)
        {
            surface->_unref();
        }
    }
};

struct vector_tile_render_baton_t
{
    uv_work_t request;
    Map* m;
    VectorTile * d;
    surface_type surface;
    mapnik::attributes variables;
    std::string error_name;
    Nan::Persistent<v8::Function> cb;
    std::string result;
    std::size_t layer_idx;
    int z;
    int x;
    int y;
    unsigned width;
    unsigned height;
    int buffer_size;
    double scale_factor;
    double scale_denominator;
    bool use_cairo;
    bool zxy_override;
    bool error;
    vector_tile_render_baton_t() :
        request(),
        m(nullptr),
        d(nullptr),
        surface(nullptr),
        variables(),
        error_name(),
        cb(),
        result(),
        layer_idx(0),
        z(0),
        x(0),
        y(0),
        width(0),
        height(0),
        buffer_size(0),
        scale_factor(1.0),
        scale_denominator(0.0),
        use_cairo(true),
        zxy_override(false),
        error(false)
        {}

    ~vector_tile_render_baton_t()
    {
        mapnik::util::apply_visitor(deref_visitor(),surface);
    }
};

struct baton_guard
{
    baton_guard(vector_tile_render_baton_t * baton) :
      baton_(baton),
      released_(false) {}

    ~baton_guard()
    {
        if (!released_) delete baton_;
    }

    void release()
    {
        released_ = true;
    }

    vector_tile_render_baton_t * baton_;
    bool released_;
};

/**
 * Render this vector tile to a surface, like a {@link mapnik.Image}
 *
 * @memberof mapnik.VectorTile
 * @name getData
 * @instance
 * @param {mapnik.Map} map object
 * @param {mapnik.Image} renderable surface
 * @param {Function} callback
 */
NAN_METHOD(VectorTile::render)
{
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    if (info.Length() < 1 || !info[0]->IsObject())
    {
        Nan::ThrowTypeError("mapnik.Map expected as first arg");
        return;
    }
    v8::Local<v8::Object> obj = info[0]->ToObject();
    if (obj->IsNull() || obj->IsUndefined() || !Nan::New(Map::constructor)->HasInstance(obj))
    {
        Nan::ThrowTypeError("mapnik.Map expected as first arg");
        return;
    }

    Map *m = Nan::ObjectWrap::Unwrap<Map>(obj);
    if (info.Length() < 2 || !info[1]->IsObject())
    {
        Nan::ThrowTypeError("a renderable mapnik object is expected as second arg");
        return;
    }
    v8::Local<v8::Object> im_obj = info[1]->ToObject();

    // ensure callback is a function
    v8::Local<v8::Value> callback = info[info.Length()-1];
    if (!info[info.Length()-1]->IsFunction())
    {
        Nan::ThrowTypeError("last argument must be a callback function");
        return;
    }

    vector_tile_render_baton_t *closure = new vector_tile_render_baton_t();
    baton_guard guard(closure);
    v8::Local<v8::Object> options = Nan::New<v8::Object>();

    if (info.Length() > 2)
    {
        if (!info[2]->IsObject())
        {
            Nan::ThrowTypeError("optional third argument must be an options object");
            return;
        }
        options = info[2]->ToObject();
        if (options->Has(Nan::New("z").ToLocalChecked()))
        {
            v8::Local<v8::Value> bind_opt = options->Get(Nan::New("z").ToLocalChecked());
            if (!bind_opt->IsNumber())
            {
                Nan::ThrowTypeError("optional arg 'z' must be a number");
                return;
            }
            closure->z = bind_opt->IntegerValue();
            closure->zxy_override = true;
        }
        if (options->Has(Nan::New("x").ToLocalChecked()))
        {
            v8::Local<v8::Value> bind_opt = options->Get(Nan::New("x").ToLocalChecked());
            if (!bind_opt->IsNumber())
            {
                Nan::ThrowTypeError("optional arg 'x' must be a number");
                return;
            }
            closure->x = bind_opt->IntegerValue();
            closure->zxy_override = true;
        }
        if (options->Has(Nan::New("y").ToLocalChecked()))
        {
            v8::Local<v8::Value> bind_opt = options->Get(Nan::New("y").ToLocalChecked());
            if (!bind_opt->IsNumber())
            {
                Nan::ThrowTypeError("optional arg 'y' must be a number");
                return;
            }
            closure->y = bind_opt->IntegerValue();
            closure->zxy_override = true;
        }
        if (options->Has(Nan::New("buffer_size").ToLocalChecked()))
        {
            v8::Local<v8::Value> bind_opt = options->Get(Nan::New("buffer_size").ToLocalChecked());
            if (!bind_opt->IsNumber())
            {
                Nan::ThrowTypeError("optional arg 'buffer_size' must be a number");
                return;
            }
            closure->buffer_size = bind_opt->IntegerValue();
        }
        if (options->Has(Nan::New("scale").ToLocalChecked()))
        {
            v8::Local<v8::Value> bind_opt = options->Get(Nan::New("scale").ToLocalChecked());
            if (!bind_opt->IsNumber())
            {
                Nan::ThrowTypeError("optional arg 'scale' must be a number");
                return;
            }
            closure->scale_factor = bind_opt->NumberValue();
        }
        if (options->Has(Nan::New("scale_denominator").ToLocalChecked()))
        {
            v8::Local<v8::Value> bind_opt = options->Get(Nan::New("scale_denominator").ToLocalChecked());
            if (!bind_opt->IsNumber())
            {
                Nan::ThrowTypeError("optional arg 'scale_denominator' must be a number");
                return;
            }
            closure->scale_denominator = bind_opt->NumberValue();
        }
        if (options->Has(Nan::New("variables").ToLocalChecked()))
        {
            v8::Local<v8::Value> bind_opt = options->Get(Nan::New("variables").ToLocalChecked());
            if (!bind_opt->IsObject())
            {
                Nan::ThrowTypeError("optional arg 'variables' must be an object");
                return;
            }
            object_to_container(closure->variables,bind_opt->ToObject());
        }
    }

    closure->layer_idx = 0;
    if (Nan::New(Image::constructor)->HasInstance(im_obj))
    {
        Image *im = Nan::ObjectWrap::Unwrap<Image>(im_obj);
        im->_ref();
        closure->width = im->get()->width();
        closure->height = im->get()->height();
        closure->surface = im;
    }
    else if (Nan::New(CairoSurface::constructor)->HasInstance(im_obj))
    {
        CairoSurface *c = Nan::ObjectWrap::Unwrap<CairoSurface>(im_obj);
        c->_ref();
        closure->width = c->width();
        closure->height = c->height();
        closure->surface = c;
        if (options->Has(Nan::New("renderer").ToLocalChecked()))
        {
            v8::Local<v8::Value> renderer = options->Get(Nan::New("renderer").ToLocalChecked());
            if (!renderer->IsString() )
            {
                Nan::ThrowError("'renderer' option must be a string of either 'svg' or 'cairo'");
                return;
            }
            std::string renderer_name = TOSTR(renderer);
            if (renderer_name == "cairo")
            {
                closure->use_cairo = true;
            }
            else if (renderer_name == "svg")
            {
                closure->use_cairo = false;
            }
            else
            {
                Nan::ThrowError("'renderer' option must be a string of either 'svg' or 'cairo'");
                return;
            }
        }
    }
#if defined(GRID_RENDERER)
    else if (Nan::New(Grid::constructor)->HasInstance(im_obj))
    {
        Grid *g = Nan::ObjectWrap::Unwrap<Grid>(im_obj);
        g->_ref();
        closure->width = g->get()->width();
        closure->height = g->get()->height();
        closure->surface = g;

        std::size_t layer_idx = 0;

        // grid requires special options for now
        if (!options->Has(Nan::New("layer").ToLocalChecked()))
        {
            Nan::ThrowTypeError("'layer' option required for grid rendering and must be either a layer name(string) or layer index (integer)");
            return;
        }
        else 
        {
            std::vector<mapnik::layer> const& layers = m->get()->layers();
            v8::Local<v8::Value> layer_id = options->Get(Nan::New("layer").ToLocalChecked());
            if (layer_id->IsString())
            {
                bool found = false;
                unsigned int idx(0);
                std::string layer_name = TOSTR(layer_id);
                for (mapnik::layer const& lyr : layers)
                {
                    if (lyr.name() == layer_name)
                    {
                        found = true;
                        layer_idx = idx;
                        break;
                    }
                    ++idx;
                }
                if (!found)
                {
                    std::ostringstream s;
                    s << "Layer name '" << layer_name << "' not found";
                    Nan::ThrowTypeError(s.str().c_str());
                    return;
                }
            }
            else if (layer_id->IsNumber())
            {
                layer_idx = layer_id->IntegerValue();
                std::size_t layer_num = layers.size();
                if (layer_idx >= layer_num)
                {
                    std::ostringstream s;
                    s << "Zero-based layer index '" << layer_idx << "' not valid, ";
                    if (layer_num > 0)
                    {
                        s << "only '" << layer_num << "' layers exist in map";
                    }
                    else
                    {
                        s << "no layers found in map";
                    }
                    Nan::ThrowTypeError(s.str().c_str());
                    return;
                }
            }
            else
            {
                Nan::ThrowTypeError("'layer' option required for grid rendering and must be either a layer name(string) or layer index (integer)");
                return;
            }
        }
        if (options->Has(Nan::New("fields").ToLocalChecked()))
        {
            v8::Local<v8::Value> param_val = options->Get(Nan::New("fields").ToLocalChecked());
            if (!param_val->IsArray())
            {
                Nan::ThrowTypeError("option 'fields' must be an array of strings");
                return;
            }
            v8::Local<v8::Array> a = v8::Local<v8::Array>::Cast(param_val);
            unsigned int i = 0;
            unsigned int num_fields = a->Length();
            while (i < num_fields)
            {
                v8::Local<v8::Value> name = a->Get(i);
                if (name->IsString())
                {
                    g->get()->add_field(TOSTR(name));
                }
                ++i;
            }
        }
        closure->layer_idx = layer_idx;
    }
#endif
    else
    {
        Nan::ThrowTypeError("renderable mapnik object expected as second arg");
        return;
    }
    closure->request.data = closure;
    closure->d = d;
    closure->m = m;
    closure->error = false;
    closure->cb.Reset(callback.As<v8::Function>());
    uv_queue_work(uv_default_loop(), &closure->request, EIO_RenderTile, (uv_after_work_cb)EIO_AfterRenderTile);
    m->_ref();
    d->Ref();
    guard.release();
    return;
}

template <typename Renderer> void process_layers(Renderer & ren,
                                            mapnik::request const& m_req,
                                            mapnik::projection const& map_proj,
                                            std::vector<mapnik::layer> const& layers,
                                            double scale_denom,
                                            std::string const& map_srs,
                                            vector_tile_render_baton_t *closure)
{
    for (auto const& lyr : layers)
    {
        if (lyr.visible(scale_denom))
        {
            protozero::pbf_reader layer_msg;
            if (closure->d->get_tile()->layer_reader(lyr.name(), layer_msg))
            {
                mapnik::layer lyr_copy(lyr);
                lyr_copy.set_srs(map_srs);
                std::shared_ptr<mapnik::vector_tile_impl::tile_datasource_pbf> ds = std::make_shared<
                                                mapnik::vector_tile_impl::tile_datasource_pbf>(
                                                    layer_msg,
                                                    closure->d->get_tile()->x(),
                                                    closure->d->get_tile()->y(),
                                                    closure->d->get_tile()->z());
                ds->set_envelope(m_req.get_buffered_extent());
                lyr_copy.set_datasource(ds);
                std::set<std::string> names;
                ren.apply_to_layer(lyr_copy,
                                   ren,
                                   map_proj,
                                   m_req.scale(),
                                   scale_denom,
                                   m_req.width(),
                                   m_req.height(),
                                   m_req.extent(),
                                   m_req.buffer_size(),
                                   names);
            }
        }
    }
}

void VectorTile::EIO_RenderTile(uv_work_t* req)
{
    vector_tile_render_baton_t *closure = static_cast<vector_tile_render_baton_t *>(req->data);

    try
    {
        mapnik::Map const& map_in = *closure->m->get();
        mapnik::vector_tile_impl::spherical_mercator merc(closure->d->tile_size());
        double minx,miny,maxx,maxy;
        if (closure->zxy_override)
        {
            merc.xyz(closure->x,closure->y,closure->z,minx,miny,maxx,maxy);
        } 
        else 
        {
            merc.xyz(closure->d->get_tile()->x(),
                     closure->d->get_tile()->y(),
                     closure->d->get_tile()->z(),
                     minx,miny,maxx,maxy);
        }
        mapnik::box2d<double> map_extent(minx,miny,maxx,maxy);
        mapnik::request m_req(closure->width, closure->height, map_extent);
        m_req.set_buffer_size(closure->buffer_size);
        mapnik::projection map_proj(map_in.srs(),true);
        double scale_denom = closure->scale_denominator;
        if (scale_denom <= 0.0)
        {
            scale_denom = mapnik::scale_denominator(m_req.scale(),map_proj.is_geographic());
        }
        scale_denom *= closure->scale_factor;
        std::vector<mapnik::layer> const& layers = map_in.layers();
#if defined(GRID_RENDERER)
        // render grid for layer
        if (closure->surface.is<Grid *>())
        {
            Grid * g = mapnik::util::get<Grid *>(closure->surface);
            mapnik::grid_renderer<mapnik::grid> ren(map_in,
                                                    m_req,
                                                    closure->variables,
                                                    *(g->get()),
                                                    closure->scale_factor);
            ren.start_map_processing(map_in);

            mapnik::layer const& lyr = layers[closure->layer_idx];
            if (lyr.visible(scale_denom))
            {
                protozero::pbf_reader layer_msg;
                if (closure->d->get_tile()->layer_reader(lyr.name(),layer_msg))
                {
                    // copy field names
                    std::set<std::string> attributes = g->get()->get_fields();
                    
                    // todo - make this a static constant
                    std::string known_id_key = "__id__";
                    if (attributes.find(known_id_key) != attributes.end())
                    {
                        attributes.erase(known_id_key);
                    }
                    std::string join_field = g->get()->get_key();
                    if (known_id_key != join_field &&
                        attributes.find(join_field) == attributes.end())
                    {
                        attributes.insert(join_field);
                    }

                    mapnik::layer lyr_copy(lyr);
                    lyr_copy.set_srs(map_in.srs());
                    std::shared_ptr<mapnik::vector_tile_impl::tile_datasource_pbf> ds = std::make_shared<
                                                    mapnik::vector_tile_impl::tile_datasource_pbf>(
                                                        layer_msg,
                                                        closure->d->get_tile()->x(),
                                                        closure->d->get_tile()->y(),
                                                        closure->d->get_tile()->z());
                    ds->set_envelope(m_req.get_buffered_extent());
                    lyr_copy.set_datasource(ds);
                    ren.apply_to_layer(lyr_copy,
                                       ren,
                                       map_proj,
                                       m_req.scale(),
                                       scale_denom,
                                       m_req.width(),
                                       m_req.height(),
                                       m_req.extent(),
                                       m_req.buffer_size(),
                                       attributes);
                }
                ren.end_map_processing(map_in);
            }
        }
        else
#endif
        if (closure->surface.is<CairoSurface *>())
        {
            CairoSurface * c = mapnik::util::get<CairoSurface *>(closure->surface);
            if (closure->use_cairo)
            {
#if defined(HAVE_CAIRO)
                mapnik::cairo_surface_ptr surface;
                // TODO - support any surface type
                surface = mapnik::cairo_surface_ptr(cairo_svg_surface_create_for_stream(
                                                       (cairo_write_func_t)c->write_callback,
                                                       (void*)(&c->ss_),
                                                       static_cast<double>(c->width()),
                                                       static_cast<double>(c->height())
                                                    ),mapnik::cairo_surface_closer());
                mapnik::cairo_ptr c_context = mapnik::create_context(surface);
                mapnik::cairo_renderer<mapnik::cairo_ptr> ren(map_in,m_req,
                                                                closure->variables,
                                                                c_context,closure->scale_factor);
                ren.start_map_processing(map_in);
                process_layers(ren,m_req,map_proj,layers,scale_denom,map_in.srs(),closure);
                ren.end_map_processing(map_in);
#else
                closure->error = true;
                closure->error_name = "no support for rendering svg with cairo backend";
#endif
            }
            else
            {
#if defined(SVG_RENDERER)
                typedef mapnik::svg_renderer<std::ostream_iterator<char> > svg_ren;
                std::ostream_iterator<char> output_stream_iterator(c->ss_);
                svg_ren ren(map_in, m_req,
                            closure->variables,
                            output_stream_iterator, closure->scale_factor);
                ren.start_map_processing(map_in);
                process_layers(ren,m_req,map_proj,layers,scale_denom,map_in.srs(),closure);
                ren.end_map_processing(map_in);
#else
                closure->error = true;
                closure->error_name = "no support for rendering svg with native svg backend (-DSVG_RENDERER)";
#endif
            }
        }
        // render all layers with agg
        else if (closure->surface.is<Image *>())
        {
            Image * js_image = mapnik::util::get<Image *>(closure->surface);
            mapnik::image_any & im = *(js_image->get());
            if (im.is<mapnik::image_rgba8>())
            {
                mapnik::image_rgba8 & im_data = mapnik::util::get<mapnik::image_rgba8>(im);
                mapnik::agg_renderer<mapnik::image_rgba8> ren(map_in,m_req,
                                                        closure->variables,
                                                        im_data,closure->scale_factor);
                ren.start_map_processing(map_in);
                process_layers(ren,m_req,map_proj,layers,scale_denom,map_in.srs(),closure);
                ren.end_map_processing(map_in);
            }
            else
            {
                throw std::runtime_error("This image type is not currently supported for rendering.");
            }
        }
    }
    catch (std::exception const& ex)
    {
        closure->error = true;
        closure->error_name = ex.what();
    }
}

void VectorTile::EIO_AfterRenderTile(uv_work_t* req)
{
    Nan::HandleScope scope;
    vector_tile_render_baton_t *closure = static_cast<vector_tile_render_baton_t *>(req->data);
    if (closure->error)
    {
        v8::Local<v8::Value> argv[1] = { Nan::Error(closure->error_name.c_str()) };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 1, argv);
    }
    else
    {
        if (closure->surface.is<Image *>())
        {
            v8::Local<v8::Value> argv[2] = { Nan::Null(), mapnik::util::get<Image *>(closure->surface)->handle() };
            Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 2, argv);
        }
#if defined(GRID_RENDERER)
        else if (closure->surface.is<Grid *>())
        {
            v8::Local<v8::Value> argv[2] = { Nan::Null(), mapnik::util::get<Grid *>(closure->surface)->handle() };
            Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 2, argv);
        }
#endif
        else if (closure->surface.is<CairoSurface *>())
        {
            v8::Local<v8::Value> argv[2] = { Nan::Null(), mapnik::util::get<CairoSurface *>(closure->surface)->handle() };
            Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 2, argv);
        }
    }

    closure->m->_unref();
    closure->d->Unref();
    closure->cb.Reset();
    delete closure;
}

NAN_METHOD(VectorTile::clearSync)
{
    info.GetReturnValue().Set(_clearSync(info));
}

v8::Local<v8::Value> VectorTile::_clearSync(Nan::NAN_METHOD_ARGS_TYPE info)
{
    Nan::EscapableHandleScope scope;
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    d->clear();
    return scope.Escape(Nan::Undefined());
}

typedef struct
{
    uv_work_t request;
    VectorTile* d;
    std::string format;
    bool error;
    std::string error_name;
    Nan::Persistent<v8::Function> cb;
} clear_vector_tile_baton_t;

/**
 * Remove all data from this vector tile
 *
 * @memberof mapnik.VectorTile
 * @name clear
 * @instance
 * @param {Function} callback
 */
NAN_METHOD(VectorTile::clear)
{
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());

    if (info.Length() == 0)
    {
        info.GetReturnValue().Set(_clearSync(info));
        return;
    }
    // ensure callback is a function
    v8::Local<v8::Value> callback = info[info.Length() - 1];
    if (!callback->IsFunction())
    {
        Nan::ThrowTypeError("last argument must be a callback function");
        return;
    }
    clear_vector_tile_baton_t *closure = new clear_vector_tile_baton_t();
    closure->request.data = closure;
    closure->d = d;
    closure->error = false;
    closure->cb.Reset(callback.As<v8::Function>());
    uv_queue_work(uv_default_loop(), &closure->request, EIO_Clear, (uv_after_work_cb)EIO_AfterClear);
    d->Ref();
    return;
}

void VectorTile::EIO_Clear(uv_work_t* req)
{
    clear_vector_tile_baton_t *closure = static_cast<clear_vector_tile_baton_t *>(req->data);
    try
    {
        closure->d->clear();
    }
    catch(std::exception const& ex)
    {
        // No reason this should ever throw an exception, not currently testable.
        // LCOV_EXCL_START
        closure->error = true;
        closure->error_name = ex.what();
        // LCOV_EXCL_STOP
    }
}

void VectorTile::EIO_AfterClear(uv_work_t* req)
{
    Nan::HandleScope scope;
    clear_vector_tile_baton_t *closure = static_cast<clear_vector_tile_baton_t *>(req->data);
    if (closure->error)
    {
        // No reason this should ever throw an exception, not currently testable.
        // LCOV_EXCL_START
        v8::Local<v8::Value> argv[1] = { Nan::Error(closure->error_name.c_str()) };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 1, argv);
        // LCOV_EXCL_STOP
    }
    else
    {
        v8::Local<v8::Value> argv[1] = { Nan::Null() };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 1, argv);
    }
    closure->d->Unref();
    closure->cb.Reset();
    delete closure;
}

#if BOOST_VERSION >= 105800

struct not_simple_feature
{
    not_simple_feature(std::string const& layer_, 
                       std::int64_t feature_id_)
        : layer(layer_),
          feature_id(feature_id_) {} 
    std::string const layer;
    std::int64_t const feature_id;
};

struct not_valid_feature
{
    not_valid_feature(std::string const& message_,
                      std::string const& layer_, 
                      std::int64_t feature_id_)
        : message(message_),
          layer(layer_),
          feature_id(feature_id_) {} 
    std::string const message;
    std::string const layer;
    std::int64_t const feature_id;
};

void layer_not_simple(protozero::pbf_reader const& layer_msg,
               unsigned x,
               unsigned y,
               unsigned z,
               std::vector<not_simple_feature> & errors)
{
    mapnik::vector_tile_impl::tile_datasource_pbf ds(layer_msg, x, y, z);
    mapnik::query q(mapnik::box2d<double>(std::numeric_limits<double>::lowest(),
                                          std::numeric_limits<double>::lowest(),
                                          std::numeric_limits<double>::max(),
                                          std::numeric_limits<double>::max()));
    mapnik::layer_descriptor ld = ds.get_descriptor();
    for (auto const& item : ld.get_descriptors())
    {
        q.add_property_name(item.get_name());
    }
    mapnik::featureset_ptr fs = ds.features(q);
    if (fs)
    {
        mapnik::feature_ptr feature;
        while ((feature = fs->next()))
        {
            if (!mapnik::geometry::is_simple(feature->get_geometry()))
            {
                errors.emplace_back(ds.get_name(), feature->id());
            }
        }
    }
}

void layer_not_valid(protozero::pbf_reader const& layer_msg,
               unsigned x,
               unsigned y,
               unsigned z,
               std::vector<not_valid_feature> & errors)
{
    mapnik::vector_tile_impl::tile_datasource_pbf ds(layer_msg, x, y, z);
    mapnik::query q(mapnik::box2d<double>(std::numeric_limits<double>::lowest(),
                                          std::numeric_limits<double>::lowest(),
                                          std::numeric_limits<double>::max(),
                                          std::numeric_limits<double>::max()));
    mapnik::layer_descriptor ld = ds.get_descriptor();
    for (auto const& item : ld.get_descriptors())
    {
        q.add_property_name(item.get_name());
    }
    mapnik::featureset_ptr fs = ds.features(q);
    if (fs)
    {
        mapnik::feature_ptr feature;
        while ((feature = fs->next()))
        {
            std::string message;
            if (!mapnik::geometry::is_valid(feature->get_geometry(), message))
            {
                errors.emplace_back(message,
                                    ds.get_name(),
                                    feature->id());
            }
        }
    }
}

void vector_tile_not_simple(VectorTile * v,
                            std::vector<not_simple_feature> & errors)
{
    protozero::pbf_reader tile_msg(v->get_tile()->get_reader());
    while (tile_msg.next(3))
    {
        protozero::pbf_reader layer_msg(tile_msg.get_message());
        layer_not_simple(layer_msg,
                         v->get_tile()->x(),
                         v->get_tile()->y(),
                         v->get_tile()->z(),
                         errors);
    }
}

v8::Local<v8::Array> make_not_simple_array(std::vector<not_simple_feature> & errors) 
{
    Nan::EscapableHandleScope scope;
    v8::Local<v8::Array> array = Nan::New<v8::Array>(errors.size());
    v8::Local<v8::String> layer_key = Nan::New<v8::String>("layer").ToLocalChecked();
    v8::Local<v8::String> feature_id_key = Nan::New<v8::String>("featureId").ToLocalChecked();
    std::uint32_t idx = 0;
    for (auto const& error : errors)
    {
        v8::Local<v8::Object> obj = Nan::New<v8::Object>();
        obj->Set(layer_key, Nan::New<v8::String>(error.layer).ToLocalChecked());
        obj->Set(feature_id_key, Nan::New<v8::Number>(error.feature_id));
        array->Set(idx++, obj);
    }
    return scope.Escape(array);
}

void vector_tile_not_valid(VectorTile * v,
                           std::vector<not_valid_feature> & errors)
{
    protozero::pbf_reader tile_msg(v->get_tile()->get_reader());
    while (tile_msg.next(3))
    {
        protozero::pbf_reader layer_msg(tile_msg.get_message());
        layer_not_valid(layer_msg,
                        v->get_tile()->x(),
                        v->get_tile()->y(),
                        v->get_tile()->z(),
                        errors);
    }
}

v8::Local<v8::Array> make_not_valid_array(std::vector<not_valid_feature> & errors) 
{
    Nan::EscapableHandleScope scope;
    v8::Local<v8::Array> array = Nan::New<v8::Array>(errors.size());
    v8::Local<v8::String> layer_key = Nan::New<v8::String>("layer").ToLocalChecked();
    v8::Local<v8::String> feature_id_key = Nan::New<v8::String>("featureId").ToLocalChecked();
    v8::Local<v8::String> message_key = Nan::New<v8::String>("message").ToLocalChecked();
    std::uint32_t idx = 0;
    for (auto const& error : errors)
    {
        v8::Local<v8::Object> obj = Nan::New<v8::Object>();
        obj->Set(layer_key, Nan::New<v8::String>(error.layer).ToLocalChecked());
        obj->Set(message_key, Nan::New<v8::String>(error.message).ToLocalChecked());
        obj->Set(feature_id_key, Nan::New<v8::Number>(error.feature_id));
        array->Set(idx++, obj);
    }
    return scope.Escape(array);
}


struct not_simple_baton
{
    uv_work_t request;
    VectorTile* v;
    bool error;
    std::vector<not_simple_feature> result;
    std::string err_msg;
    Nan::Persistent<v8::Function> cb;
};

struct not_valid_baton
{
    uv_work_t request;
    VectorTile* v;
    bool error;
    std::vector<not_valid_feature> result;
    std::string err_msg;
    Nan::Persistent<v8::Function> cb;
};

/**
 * Count the number of geometries that are not OGC simple
 *
 * @memberof mapnik.VectorTile
 * @name reportGeometrySimplicitySync
 * @instance
 * @returns {number} number of features that are not simple 
 */
NAN_METHOD(VectorTile::reportGeometrySimplicitySync)
{
    info.GetReturnValue().Set(_reportGeometrySimplicitySync(info));
}

v8::Local<v8::Value> VectorTile::_reportGeometrySimplicitySync(Nan::NAN_METHOD_ARGS_TYPE info)
{
    Nan::EscapableHandleScope scope;
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    try
    {
        std::vector<not_simple_feature> errors;
        vector_tile_not_simple(d, errors);
        return scope.Escape(make_not_simple_array(errors));
    }
    catch (std::exception const& ex)
    {
        Nan::ThrowError(ex.what());
    }
    return scope.Escape(Nan::Undefined());
}

/**
 * Count the number of geometries that are not OGC valid
 *
 * @memberof mapnik.VectorTile
 * @name reportGeometryValiditySync
 * @instance
 * @returns {number} number of features that are not valid
 */
NAN_METHOD(VectorTile::reportGeometryValiditySync)
{
    info.GetReturnValue().Set(_reportGeometryValiditySync(info));
}

v8::Local<v8::Value> VectorTile::_reportGeometryValiditySync(Nan::NAN_METHOD_ARGS_TYPE info)
{
    Nan::EscapableHandleScope scope;
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    try
    {
        std::vector<not_valid_feature> errors;
        vector_tile_not_valid(d, errors);
        return scope.Escape(make_not_valid_array(errors));
    }
    catch (std::exception const& ex)
    {
        Nan::ThrowError(ex.what());
    }
    return scope.Escape(Nan::Undefined());
}

/**
 * Count the number of non OGC simple geometries
 *
 * @memberof mapnik.VectorTile
 * @name reportGeometrySimplicity
 * @instance
 * @param {Function} callback
 */
NAN_METHOD(VectorTile::reportGeometrySimplicity)
{
    if (info.Length() == 0)
    {
        info.GetReturnValue().Set(_reportGeometrySimplicitySync(info));
        return;
    }
    // ensure callback is a function
    v8::Local<v8::Value> callback = info[info.Length() - 1];
    if (!callback->IsFunction())
    {
        Nan::ThrowTypeError("last argument must be a callback function");
        return;
    }

    not_simple_baton *closure = new not_simple_baton();
    closure->request.data = closure;
    closure->v = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    closure->error = false;
    closure->cb.Reset(callback.As<v8::Function>());
    uv_queue_work(uv_default_loop(), &closure->request, EIO_ReportGeometrySimplicity, (uv_after_work_cb)EIO_AfterReportGeometrySimplicity);
    closure->v->Ref();
    return;
}

void VectorTile::EIO_ReportGeometrySimplicity(uv_work_t* req)
{
    not_simple_baton *closure = static_cast<not_simple_baton *>(req->data);
    try
    {
        vector_tile_not_simple(closure->v, closure->result);
    }
    catch (std::exception const& ex)
    {
        closure->error = true;
        closure->err_msg = ex.what();
    }
}

void VectorTile::EIO_AfterReportGeometrySimplicity(uv_work_t* req)
{
    Nan::HandleScope scope;
    not_simple_baton *closure = static_cast<not_simple_baton *>(req->data);
    if (closure->error) 
    {
        v8::Local<v8::Value> argv[1] = { Nan::Error(closure->err_msg.c_str()) };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 1, argv);
    }
    else
    {
        v8::Local<v8::Array> array = make_not_simple_array(closure->result);
        v8::Local<v8::Value> argv[2] = { Nan::Null(), array };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 2, argv);
    }
    closure->v->Unref();
    closure->cb.Reset();
    delete closure;
}

/**
 * Count the number of non OGC valid geometries
 *
 * @memberof mapnik.VectorTile
 * @name reportGeometryValidity
 * @instance
 * @param {Function} callback
 */
NAN_METHOD(VectorTile::reportGeometryValidity)
{
    if (info.Length() == 0)
    {
        info.GetReturnValue().Set(_reportGeometryValiditySync(info));
        return;
    }
    // ensure callback is a function
    v8::Local<v8::Value> callback = info[info.Length() - 1];
    if (!callback->IsFunction())
    {
        Nan::ThrowTypeError("last argument must be a callback function");
        return;
    }

    not_valid_baton *closure = new not_valid_baton();
    closure->request.data = closure;
    closure->v = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    closure->error = false;
    closure->cb.Reset(callback.As<v8::Function>());
    uv_queue_work(uv_default_loop(), &closure->request, EIO_ReportGeometryValidity, (uv_after_work_cb)EIO_AfterReportGeometryValidity);
    closure->v->Ref();
    return;
}

void VectorTile::EIO_ReportGeometryValidity(uv_work_t* req)
{
    not_valid_baton *closure = static_cast<not_valid_baton *>(req->data);
    try
    {
        vector_tile_not_valid(closure->v, closure->result);
    }
    catch (std::exception const& ex)
    {
        closure->error = true;
        closure->err_msg = ex.what();
    }
}

void VectorTile::EIO_AfterReportGeometryValidity(uv_work_t* req)
{
    Nan::HandleScope scope;
    not_valid_baton *closure = static_cast<not_valid_baton *>(req->data);
    if (closure->error) 
    {
        v8::Local<v8::Value> argv[1] = { Nan::Error(closure->err_msg.c_str()) };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 1, argv);
    }
    else
    {
        v8::Local<v8::Array> array = make_not_valid_array(closure->result);
        v8::Local<v8::Value> argv[2] = { Nan::Null(), array };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(closure->cb), 2, argv);
    }
    closure->v->Unref();
    closure->cb.Reset();
    delete closure;
}

#endif // BOOST_VERSION >= 1.58

NAN_GETTER(VectorTile::get_tile_size)
{
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    info.GetReturnValue().Set(Nan::New<v8::Number>(d->tile_->tile_size()));
}

NAN_GETTER(VectorTile::get_buffer_size)
{
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    info.GetReturnValue().Set(Nan::New<v8::Number>(d->tile_->buffer_size()));
}

NAN_SETTER(VectorTile::set_tile_size)
{
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    if (!value->IsNumber())
    {
        Nan::ThrowError("Must provide a number");
    } 
    else 
    {
        double val = value->NumberValue();
        if (val <= 0.0)
        {
            Nan::ThrowError("tile size must be greater then zero");
            return;
        }
        d->tile_->tile_size(val);
    }
}

NAN_SETTER(VectorTile::set_buffer_size)
{
    VectorTile* d = Nan::ObjectWrap::Unwrap<VectorTile>(info.Holder());
    if (!value->IsNumber())
    {
        Nan::ThrowError("Must provide a number");
    } 
    else 
    {
        double val = value->NumberValue();
        if (static_cast<double>(d->tile_size()) + (2 * val) <= 0)
        {
            Nan::ThrowError("too large of a negative buffer for tilesize");
            return;
        }
        d->tile_->buffer_size(val);
    }
}
