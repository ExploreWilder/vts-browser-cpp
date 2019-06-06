/**
 * Copyright (c) 2017 Melown Technologies SE
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * *  Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <optick.h>
#include <utf8.h>

#include "../include/vts-browser/exceptions.hpp"
#include "../include/vts-browser/log.hpp"

#include "../utilities/json.hpp"
#include "../utilities/case.hpp"
#include "../gpuResource.hpp"
#include "../geodata.hpp"
#include "../renderTasks.hpp"
#include "../mapConfig.hpp"
#include "../map.hpp"

namespace vts
{

using Json::Value;

namespace
{

typedef std::map<std::string, const Value> AmpVariables;
typedef std::basic_string<uint32> S32;

S32 s8to32(const std::string &s8)
{
    S32 s32;
    s32.reserve(s8.size());
    utf8::utf8to32(s8.begin(), s8.end(), std::back_inserter(s32));
    return s32;
}

std::string s32to8(const S32 &s32)
{
    std::string s8;
    s8.reserve(s32.size());
    utf8::utf32to8(s32.begin(), s32.end(), std::back_inserter(s8));
    return s8;
}

struct AmpVarsScope
{
    AmpVariables &vars;
    AmpVariables cache;

    AmpVarsScope(AmpVariables &vars) : vars(vars)
    {
        std::swap(vars, cache);
    }

    ~AmpVarsScope()
    {
        std::swap(vars, cache);
    }
};

struct GpuGeodataSpecComparator
{
    bool operator () (const GpuGeodataSpec &a, const GpuGeodataSpec &b) const
    {
        if (a.type != b.type)
            return a.type < b.type;
        if (a.bitmap != b.bitmap)
            return a.bitmap < b.bitmap;
        int c = memcmp(&a.commonData, &b.commonData, sizeof(a.commonData));
        if (c != 0)
            return c < 0;
        int s = memcmp(&a.unionData, &b.unionData, sizeof(a.unionData));
        if (s != 0)
            return s < 0;
        int m = memcmp(a.model, b.model, sizeof(a.model));
        if (m != 0)
            return m < 0;
        return a.fontCascade < b.fontCascade;
    }
};

#define THROW LOGTHROW(err3, GeodataValidationException)

bool hasLatin(const std::string &s)
{
    auto it = s.begin();
    const auto e = s.end();
    while (it != e)
    {
        uint32 c = utf8::next(it, e);
        if ((c >= 0x41 && c <= 0x5a)
            || (c >= 0x61 && c <= 0x7a)
            || ((c >= 0xc0 && c <= 0xff) && c != 0xd7 && c != 0xf7)
            || (c >= 0x100 && c <= 0x17f))
            return true;
    }
    return false;
}

bool isCjk(const std::string &s)
{
    auto it = s.begin();
    const auto e = s.end();
    while (it != e)
    {
        uint32 c = utf8::next(it, e);
        if (!((c >= 0x4E00 && c <= 0x62FF) || (c >= 0x6300 && c <= 0x77FF) ||
            (c >= 0x7800 && c <= 0x8CFF) || (c >= 0x8D00 && c <= 0x9FFF) ||
            (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x20000 && c <= 0x215FF) ||
            (c >= 0x21600 && c <= 0x230FF) || (c >= 0x23100 && c <= 0x245FF) ||
            (c >= 0x24600 && c <= 0x260FF) || (c >= 0x26100 && c <= 0x275FF) ||
            (c >= 0x27600 && c <= 0x290FF) || (c >= 0x29100 && c <= 0x2A6DF) ||
            (c >= 0x2A700 && c <= 0x2B73F) || (c >= 0x2B740 && c <= 0x2B81F) ||
            (c >= 0x2B820 && c <= 0x2CEAF) || (c >= 0x2CEB0 && c <= 0x2EBEF) ||
            (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x3300 && c <= 0x33FF) ||
            (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xF900 && c <= 0xFAFF) ||
            (c >= 0x2F800 && c <= 0x2FA1F) ||
            (c <= 0x40) || (c >= 0xa0 && c <= 0xbf)))
            return false;
    }
    return true;
}

uint32 utf8len(const std::string &s)
{
    return utf8::distance(s.begin(), s.end());
}

std::string utf8trim(const std::string &sin)
{
    auto s = s8to32(sin);
    while (!s.empty() && isWhitespace(s[s.size() - 1]))
        s.pop_back();
    while (!s.empty() && isWhitespace(s[0]))
        s.erase(s.begin());
    return s32to8(s);
}

sint32 utf8find(const std::string &str, const std::string &what, uint32 off)
{
    S32 s = s8to32(str);
    S32 w = s8to32(what);
    auto p = s.find(w, off);
    if (p == s.npos)
        return -1;
    return p;
}

std::string utf8replace(const std::string &str, const std::string &what,
    const std::string &replacement)
{
    S32 s = s8to32(str);
    S32 w = s8to32(what);
    S32 r = s8to32(replacement);
    auto p = s.find(w);
    if (p == s.npos)
        return str;
    s = s.replace(p, what.length(), r);
    return s32to8(s);
}

std::string utf8substr(const std::string &str, sint32 start, uint32 length)
{
    S32 s = s8to32(str);
    if (start >= (sint32)s.size())
        return "";
    if (start < 0)
    {
        if (-start >= (sint32)s.size())
            start = 0;
        else
            start += s.size();
    }
    s = s.substr(start, length);
    return s32to8(s);
}

double str2num(const std::string &s)
{
    if (s.empty())
        return nan1();
    std::stringstream ss(s);
    double f = nan1();
    ss >> std::noskipws >> f;
    if ((ss.rdstate() ^ std::ios_base::eofbit))
    {
        THROW << "Failed to convert string <"
            << s << "> to number";
    }
    return f;
}

bool isLayerStyleRequested(const Value &v)
{
    if (v.isNull())
        return false;
    if (v.isConvertibleTo(Json::ValueType::booleanValue))
        return v.asBool();
    return true;
}

template<bool Validating>
struct geoContext
{
    enum class Type
    {
        Point,
        Line,
        Polygon,
    };

    typedef std::array<float, 3> Point;

    double convertToDouble(const Value &p) const
    {
        Value v = evaluate(p);
        if (v.isString())
            return str2num(v.asString());
        return v.asDouble();
    }

    Point convertPoint(const Value &p) const
    {
        return group->convertPoint(evaluate(p));
    }

    vec4f convertColor(const Value &p) const
    {
        Value v = evaluate(p);
        validateArrayLength(v, 4, 4, "Color must have 4 components");
        return vec4f(v[0].asInt(), v[1].asInt(),
            v[2].asInt(), v[3].asInt()) / 255.f;
    }

    vec4f convertVector4(const Value &p) const
    {
        Value v = evaluate(p);
        validateArrayLength(v, 4, 4, "Expected 4 components");
        return vec4f(convertToDouble(v[0]), convertToDouble(v[1]),
            convertToDouble(v[2]), convertToDouble(v[3]));
    }

    vec2f convertVector2(const Value &p) const
    {
        Value v = evaluate(p);
        validateArrayLength(v, 2, 2, "Expected 2 components");
        return vec2f(convertToDouble(v[0]), convertToDouble(v[1]));
    }

    vec2f convertNoOverlapMargin(const Value &p) const
    {
        Json::Value lnom = evaluate(p);
        switch (lnom.size())
        {
        case 2:
        case 4:
            // 4 values are valid, but we ignore the last two now
            return {
                convertToDouble(lnom[0]),
                convertToDouble(lnom[1])
            };
        default:
            if (Validating)
                THROW << "Invalid no-overlap-margin array size";
            throw;
        }
    }

    GpuGeodataSpec::Stick convertStick(const Value &p) const
    {
        Value v = evaluate(p);
        validateArrayLength(v, 7, 8, "Stick must have 7 or 8 components");
        GpuGeodataSpec::Stick s;
        s.heightMax = convertToDouble(v[0]);
        s.heightThreshold = convertToDouble(v[1]);
        s.width = convertToDouble(v[2]);
        vecToRaw(vec4f(vec4f(v[3].asInt(), v[4].asInt(), v[5].asInt(),
            v[6].asInt()) / 255.f), s.color);
        s.offset = v.size() > 7 ? convertToDouble(v[7]) : 0;
        return s;
    }

    GpuGeodataSpec::Origin convertOrigin(const Value &p) const
    {
        Value v = evaluate(p);
        std::string s = v.asString();
        if (s == "top-left")
            return GpuGeodataSpec::Origin::TopLeft;
        if (s == "top-right")
            return GpuGeodataSpec::Origin::TopRight;
        if (s == "top-center")
            return GpuGeodataSpec::Origin::TopCenter;
        if (s == "center-left")
            return GpuGeodataSpec::Origin::CenterLeft;
        if (s == "center-right")
            return GpuGeodataSpec::Origin::CenterRight;
        if (s == "center-center")
            return GpuGeodataSpec::Origin::CenterCenter;
        if (s == "bottom-left")
            return GpuGeodataSpec::Origin::BottomLeft;
        if (s == "bottom-right")
            return GpuGeodataSpec::Origin::BottomRight;
        if (s == "bottom-center")
            return GpuGeodataSpec::Origin::BottomCenter;
        if (Validating)
            THROW << "Invalid origin";
        return GpuGeodataSpec::Origin::Invalid;
    }

    GpuGeodataSpec::TextAlign convertTextAlign(const Value &p) const
    {
        Value v = evaluate(p);
        std::string s = v.asString();
        if (s == "left")
            return GpuGeodataSpec::TextAlign::Left;
        if (s == "right")
            return GpuGeodataSpec::TextAlign::Right;
        if (s == "center")
            return GpuGeodataSpec::TextAlign::Center;
        if (Validating)
            THROW << "Invalid text align";
        return GpuGeodataSpec::TextAlign::Invalid;
    }

    static void validateArrayLength(const Value &value,
        uint32 minimum, uint32 maximum, // inclusive range
        const std::string &message)
    {
        if (Validating)
        {
            if (!value.isArray() || value.size() < minimum
                || value.size() > maximum)
                THROW << message;
        }
    }

    static bool getCompatibilityMode(const GeodataTile *data)
    {
        const Value &style = *data->style->json;
        if (style.isMember("compatibility-mode"))
            return style["compatibility-mode"].asBool();
        // todo
        // later, when a versioning scheme is determined,
        //   the default will change based on the version
        if (Validating)
        {
            if (style.isMember("version"))
                THROW << "Not implemented: specifying 'version' in geodata"
                " stylesheet should change the way in which compatibility"
                " mode is handled, but the versioning scheme"
                " is not yet determined. To solve this problem,"
                " either update the browser to a version with support"
                " for versioned stylesheets, or define 'compatibility-mode'.";
        }
        return true;
    }

    geoContext(GeodataTile *data)
        : data(data),
        stylesheet(data->style.get()),
        style(*data->style->json),
        features(stringToJson(*data->features)),
        browserOptions(*data->browserOptions),
        aabbPhys{ data->aabbPhys[0], data->aabbPhys[1] },
        lod(data->lod),
        compatibility(getCompatibilityMode(data)),
        currentLayer(nullptr)
    {}

    // defines which style layers are candidates for a specific feature type
    std::vector<std::string> filterLayersByType(Type t) const
    {
        std::vector<std::string> result;
        const auto allLayerNames = style["layers"].getMemberNames();
        for (const std::string &layerName : allLayerNames)
        {
            Value layer = style["layers"][layerName];
            if (layer.isMember("filter") && layer["filter"].isArray()
                && layer["filter"][0] == "skip")
                continue;
            if (layer.isMember("visibility-switch"))
            {
                result.push_back(layerName);
                continue;
            }
            bool point = isLayerStyleRequested(layer["point"]);
            bool line = isLayerStyleRequested(layer["line"]);
            bool icon = isLayerStyleRequested(layer["icon"]);
            bool labelScreen = isLayerStyleRequested(layer["label"]);
            bool labelFlat = isLayerStyleRequested(layer["line-label"]);
            bool polygon = isLayerStyleRequested(layer["polygon"]);
            bool ok = false;
            switch (t)
            {
            case Type::Point:
                ok = point || icon || labelScreen;
                break;
            case Type::Line:
                ok = line || labelFlat; // todo enable degrading line features to point layers
                break;
            case Type::Polygon:
                ok = polygon; // todo enable degrading polygon features to all layer types
                break;
            }
            if (ok)
                result.push_back(layerName);
        }
        return result;
    }

    // entry point
    //   processes all features with all style layers
    void process()
    {
        if (Validating)
        {
            try
            {
                return processInternal();
            }
            catch (const GeodataValidationException &)
            {
                throw;
            }
            catch (const std::runtime_error &e)
            {
                THROW << "Runtime error <" << e.what() << ">";
            }
            catch (const std::logic_error &e)
            {
                THROW << "Logic error <" << e.what() << ">";
            }
            catch (std::exception &e)
            {
                THROW << "General error <" << e.what() << ">";
            }
        }
        return processInternal();
    }

    void processInternal()
    {
        if (Validating)
        {
            // check version
            if (features["version"].asInt() != 1)
            {
                THROW << "Invalid geodata features <"
                    << data->name << "> version <"
                    << features["version"].asInt() << ">";
            }
        }

        solveInheritance();

        static const std::vector<std::pair<Type, std::string>> allTypes
            = { { Type::Point, "points" },
                { Type::Line, "lines" },
                { Type::Polygon, "polygons"}
        };

        // style layers filtered by valid feature types
        std::map<Type, std::vector<std::string>> typedLayerNames;
        for (Type t : { Type::Point, Type::Line, Type::Polygon })
            typedLayerNames[t] = filterLayersByType(t);

        // groups
        for (const Value &group : features["groups"])
        {
            this->group.emplace(group);
            // types
            for (const auto &type : allTypes)
            {
                this->type.emplace(type.first);
                const auto &layers = typedLayerNames[type.first];
                if (layers.empty())
                    continue;
                // features
                for (const Value &feature : group[type.second])
                {
                    this->feature.emplace(feature);
                    // layers
                    for (const std::string &layerName : layers)
                        processFeatureName(layerName);
                }
                this->feature.reset();
            }
            this->type.reset();
        }
        this->group.reset();

#ifndef NDEBUG
        finalValidation();
#endif // !NDEBUG

        // put cache into queue for upload
        data->specsToUpload.clear();
        for (const GpuGeodataSpec &spec : cacheData)
            data->specsToUpload.push_back(
                std::move(const_cast<GpuGeodataSpec&>(spec)));
    }

    void finalValidation()
    {
        for (const GpuGeodataSpec &spec : cacheData)
        {
            // validate that all vectors are of same length
            {
                uint32 itemsCount = 0;
                const auto &c = [&](std::size_t s)
                {
                    if (s)
                    {
                        if (itemsCount == 0)
                            itemsCount = s;
                        else if (itemsCount != s)
                        {
                            assert(false);
                        }
                    }
                };
                c(spec.positions.size());
                c(spec.iconCoords.size());
                c(spec.texts.size());
                c(spec.hysteresisIds.size());
                c(spec.importances.size());
            }
        }
    }

    Value resolveInheritance(const Value &orig) const
    {
        if (!orig["inherit"])
            return orig;

        Value base = resolveInheritance(
            style["layers"][orig["inherit"].asString()]);

        for (auto n : orig.getMemberNames())
            base[n] = orig[n];

        base.removeMember("inherit");

        return base;
    }

    // update style layers with inherit property
    void solveInheritance()
    {
        Value ls;
        for (const std::string &n : style["layers"].getMemberNames())
            ls[n] = resolveInheritance(style["layers"][n]);
        style["layers"].swap(ls);
    }

    // solves @constants, $properties, &variables and #identifiers
    Value replacement(const std::string &name) const
    {
        if (Validating)
        {
            try
            {
                return replacementInternal(name);
            }
            catch (...)
            {
                LOG(info3) << "In replacements of <" << name << ">";
                throw;
            }
        }
        return replacementInternal(name);
    }

    Value replacementInternal(const std::string &name) const
    {
        assert(group);
        assert(type);
        assert(feature);
        if (name.empty())
            return Value();
        switch (name[0])
        {
        case '@': // constant
            return evaluate(style["constants"][name]);
        case '$': // property
            return (*feature)["properties"][name.substr(1)];
        case '&': // ampersand variable
        {
            auto it = ampVariables.find(name);
            if (it != ampVariables.end())
                return it->second;
            if (Validating)
            {
                if (!currentLayer->isMember(name))
                    THROW << "Undefined variable <" << name << ">";
            }
            return const_cast<AmpVariables&>(ampVariables).emplace(
                    name, evaluate((*currentLayer)[name])).first->second;
        }
        case '#': // identifier
            if (name == "#id")
                return (*feature)["properties"]["name"];
            if (name == "#group")
                return group->group["id"];
            if (name == "#type")
            {
                switch (*type)
                {
                case Type::Point:
                    return "point";
                case Type::Line:
                    return "line";
                case Type::Polygon:
                    return "polygon";
                }
            }
            if (name == "#metric")
                return !!data->map->options.measurementUnitsSystem;
            if (name == "#language")
                return data->map->options.language;
            if (name == "#lod")
                return lod;
            if (name == "#tileSize")
            {
                // todo
            }
            return Value();
        default:
            return name;
        }
    }

    // solves functions and string expansion
    Value evaluate(const Value &expression) const
    {
        if (Validating)
        {
            try
            {
                return evaluateInternal(expression);
            }
            catch (...)
            {
                LOG(info3) << "In evaluation of <"
                    << expression.toStyledString() << ">";
                throw;
            }
        }
        return evaluateInternal(expression);
    }

    Value evaluateInternal(const Value &expression) const
    {
        if (expression.isArray())
            return evaluateArray(expression);
        if (expression.isObject())
            return evaluateObject(expression);
        if (expression.isString())
            return evaluateString(expression.asString());
        return expression;
    }

    Value interpolate(const Value &a, const Value &b, double f) const
    {
        if (Validating)
        {
            if (a.isArray() != b.isArray())
                THROW << "Cannot interpolate <" << a.toStyledString()
                << "> and <" << b.toStyledString()
                << "> because one is array and the other is not";
            if (a.isArray() && a.size() != b.size())
            {
                THROW << "Cannot interpolate <" << a.toStyledString()
                    << "> and <" << b.toStyledString()
                    << "> because they have different number of elements";
            }
        }
        if (a.isArray())
        {
            Value r;
            Json::ArrayIndex cnt = a.size();
            for (Json::ArrayIndex i = 0; i < cnt; i++)
                r[i] = interpolate(a[i], b[i], f);
            return r;
        }
        double aa = convertToDouble(a);
        double bb = convertToDouble(b);
        return aa + (bb - aa) * f;
    }

    template<bool Linear>
    Value evaluatePairsArray(const Value &what,
        const Value &searchArray) const
    {
        if (Validating)
        {
            try
            {
                return evaluatePairsArrayInternal<Linear>(what, searchArray);
            }
            catch (...)
            {
                LOG(info3) << "In search of <"
                    << what.toStyledString()
                    << "> in pairs array <"
                    << searchArray.toStyledString() << ">";
                throw;
            }
        }
        return evaluatePairsArrayInternal<Linear>(what, searchArray);
    }

    template<bool Linear>
    Value evaluatePairsArrayInternal(const Value &what,
        const Value &searchArrayParam) const
    {
        const Value searchArray = evaluate(searchArrayParam);
        if (Validating)
        {
            if (!searchArray.isArray())
                THROW << "Expected an array";
            for (const auto &p : searchArray)
            {
                if (!p.isArray() || p.size() != 2)
                    THROW << "Expected an array with two elements";
            }
        }
        double v = convertToDouble(evaluate(what));
        for (sint32 index = searchArray.size() - 1; index >= 0; index--)
        {
            double v1 = convertToDouble(searchArray[index][0]);
            if (v < v1)
                continue;
            if (Linear)
            {
                if (index + 1u < searchArray.size())
                {
                    double v2 = convertToDouble(searchArray[index + 1][0]);
                    return interpolate(
                        evaluate(searchArray[index + 0][1]),
                        evaluate(searchArray[index + 1][1]),
                        (v - v1) / (v2 - v1));
                }
            }
            return evaluate(searchArray[index][1]);
        }
        return evaluate(searchArray[0][1]);
    }

    Value evaluateMap(const Value &key,
        const Value &pairsp, const Value &default_) const
    {
        if (Validating)
        {
            try
            {
                return evaluateMapInternal(key, pairsp, default_);
            }
            catch (...)
            {
                LOG(info3) << "In search of <"
                    << key.toStyledString()
                    << "> in pairs array <"
                    << pairsp.toStyledString()
                    << "> with default value <"
                    << default_.toStyledString()
                    << ">";
                throw;
            }
        }
        return evaluateMapInternal(key, pairsp, default_);
    }

    Value evaluateMapInternal(const Value &key,
        const Value &pairsp, const Value &default_) const
    {
        const std::string k = evaluate(key).asString();
        const Value pairs = evaluate(pairsp);
        if (Validating)
        {
            if (!pairs.isArray())
                THROW << "Expected an array";
            std::set<std::string> keys;
            for (const auto &p : pairs)
            {
                if (!p.isArray() || p.size() != 2)
                    THROW << "Expected an array with two elements";
                std::string a = evaluate(p[0]).asString();
                if (!keys.insert(a).second)
                    THROW << "Duplicate keys <" << a << ">";
            }
        }
        for (const Value &p : pairs)
        {
            if (k == evaluate(p[0]).asString())
                return evaluate(p[1]);
        }
        return evaluate(default_);
    }

    Value evaluateArray(const Value &expression) const
    {
        Value r(expression);
        for (Value &it : r)
            it = evaluate(it);
        return r;
    }

    // evaluate function
    Value evaluateObject(const Value &expression) const
    {
        if (Validating)
        {
            if (expression.size() != 1)
                THROW << "Function must have exactly one member";
        }
        const std::string fnc = expression.getMemberNames()[0];

        // 'sgn', 'sin', 'cos', 'tan', 'asin', 'acos', 'atan', 'sqrt', 'abs', 'deg2rad', 'rad2deg', 'log'
        if (fnc == "sgn")
        {
            double v = convertToDouble(expression[fnc]);
            if (v < 0) return -1;
            if (v > 0) return 1;
            return 0;
        }
        if (fnc == "sin")
            return std::sin(convertToDouble(expression[fnc]));
        if (fnc == "cos")
            return std::cos(convertToDouble(expression[fnc]));
        if (fnc == "tan")
            return std::tan(convertToDouble(expression[fnc]));
        if (fnc == "asin")
            return std::asin(convertToDouble(expression[fnc]));
        if (fnc == "acos")
            return std::acos(convertToDouble(expression[fnc]));
        if (fnc == "atan")
            return std::atan(convertToDouble(expression[fnc]));
        if (fnc == "sqrt")
            return std::sqrt(convertToDouble(expression[fnc]));
        if (fnc == "abs")
            return std::abs(convertToDouble(expression[fnc]));
        if (fnc == "deg2rad")
            return convertToDouble(expression[fnc]) * M_PI / 180;
        if (fnc == "rad2deg")
            return convertToDouble(expression[fnc]) * 180 / M_PI;
        if (fnc == "log")
            return std::log(convertToDouble(expression[fnc]));

        // 'round'
        if (fnc == "round")
            return (sint32)std::round(convertToDouble(expression[fnc]));

        // 'add', 'sub', 'mul', 'div', 'pow', 'atan2'
#define COMP(NAME, OP) \
if (fnc == #NAME) \
{ \
    validateArrayLength(expression[#NAME], 2, 2, \
        "Function '" #NAME "' is expecting an array with 2 elements."); \
    double a = convertToDouble(expression[#NAME][0]); \
    double b = convertToDouble(expression[#NAME][1]); \
    return a OP b; \
}
        COMP(add, +);
        COMP(sub, -);
        COMP(mul, *);
        COMP(div, / );
#undef COMP
        if (fnc == "pow")
        {
            validateArrayLength(expression[fnc], 2, 2,
                "Function 'pow' is expecting an array with 2 elements.");
            double a = convertToDouble(expression[fnc][0]);
            double b = convertToDouble(expression[fnc][1]);
            return std::pow(a, b);
        }
        if (fnc == "atan2")
        {
            validateArrayLength(expression[fnc], 2, 2,
                "Function 'atan2' is expecting an array with 2 elements.");
            double a = convertToDouble(expression[fnc][0]);
            double b = convertToDouble(expression[fnc][1]);
            return std::atan2(a, b);
        }

        // 'clamp'
        if (fnc == "clamp")
        {
            const Value &arr = expression[fnc];
            validateArrayLength(arr, 3, 3,
                "Function 'clamp' must have 3 values");
            double f = convertToDouble(arr[0]);
            double a = convertToDouble(arr[1]);
            double b = convertToDouble(arr[2]);
            if (Validating)
            {
                if (a >= b)
                    THROW << "Function 'clamp' bound values <"
                    << a << "> and <" << b << "> are invalid";
            }
            return std::max(a, std::min(f, b));
        }

        // 'min', 'max'
        if (fnc == "min")
        {
            const Value &arr = expression[fnc];
            validateArrayLength(arr, 1, (uint32)-1,
                "Function 'min' expects an array");
            double t = convertToDouble(arr[0]);
            for (uint32 i = 1; i < arr.size(); i++)
                t = std::min(t, convertToDouble(arr[i]));
            return t;
        }
        if (fnc == "max")
        {
            const Value &arr = expression[fnc];
            validateArrayLength(arr, 1, (uint32)-1,
                "Function 'max' expects an array");
            double t = convertToDouble(arr[0]);
            for (uint32 i = 1; i < arr.size(); i++)
                t = std::max(t, convertToDouble(arr[i]));
            return t;
        }

        // 'if'
        if (fnc == "if")
        {
            const Value &arr = expression[fnc];
            validateArrayLength(arr, 3, 3,
                "Function 'if' must have 3 values");
            if (filter(arr[0]))
                return evaluate(arr[1]);
            else
                return evaluate(arr[2]);
        }

        // 'strlen', 'str2num', 'lowercase', 'uppercase', 'capitalize', 'trim'
        if (fnc == "strlen")
            return utf8len(evaluate(expression[fnc]).asString());
        if (fnc == "str2num")
            return str2num(evaluate(expression[fnc]).asString());
        if (fnc == "lowercase")
            return lowercase(evaluate(expression[fnc]).asString());
        if (fnc == "uppercase")
            return uppercase(evaluate(expression[fnc]).asString());
        if (fnc == "capitalize")
            return titlecase(evaluate(expression[fnc]).asString());
        if (fnc == "trim")
            return utf8trim(evaluate(expression[fnc]).asString());

        // 'find', 'replace', 'substr'
        if (fnc == "find")
        {
            const auto &arr = expression[fnc];
            validateArrayLength(arr, 2, 3,
                "Function 'find' must have 2 or 3 values");
            return utf8find(evaluate(arr[0]).asString(),
                evaluate(arr[1]).asString(),
                arr.size() == 3 ? evaluate(arr[2]).asUInt() : 0);
        }
        if (fnc == "replace")
        {
            const auto &arr = expression[fnc];
            validateArrayLength(arr, 3, 3,
                "Function 'replace' must have 3 values");
            return utf8replace(evaluate(arr[0]).asString(),
                evaluate(arr[1]).asString(),
                evaluate(arr[2]).asString());
        }
        if (fnc == "substr")
        {
            const auto &arr = expression[fnc];
            validateArrayLength(arr, 2, 3,
                "Function 'substr' must have 2 or 3 values");
            return utf8substr(evaluate(arr[0]).asString(),
                evaluate(arr[1]).asInt(),
                arr.size() == 3 ? evaluate(arr[2]).asUInt() : (uint32)-1);
        }

        // 'has-fonts', 'has-latin', 'is-cjk'
        if (fnc == "has-latin")
            return hasLatin(evaluate(expression[fnc]).asString());
        if (fnc == "is-cjk")
            return isCjk(evaluate(expression[fnc]).asString());

        // 'map'
        if (fnc == "map")
        {
            // { "map" : [inputValue, [[key, value], ...], defaultValue] }
            const auto &arr = expression[fnc];
            validateArrayLength(arr, 3, 3,
                "Function 'map' must have 3 values");
            return evaluateMap(arr[0], arr[1], arr[2]);
        }

        // 'discrete', 'discrete2', 'linear', 'linear2'
        if (fnc == "discrete")
            return evaluatePairsArray<false>(lod, expression[fnc]);
        if (fnc == "discrete2")
            return evaluatePairsArray<false>(expression[fnc][0],
                expression[fnc][1]);
        if (fnc == "linear")
            return evaluatePairsArray<true>(lod, expression[fnc]);
        if (fnc == "linear2")
            return evaluatePairsArray<true>(expression[fnc][0],
                expression[fnc][1]);

        // 'lod-scaled'
        if (fnc == "lod-scaled")
        {
            Value arr = evaluate(expression["lod-scaled"]);
            validateArrayLength(arr, 2, 3,
                "Function 'lod-scaled' must have 2 or 3 values");
            float l = convertToDouble(arr[0]);
            float v = convertToDouble(arr[1]);
            float bf = arr.size() == 3 ? convertToDouble(arr[2]) : 1;
            return Value(std::pow(2 * bf, l - lod) * v);
        }

        // 'log-scale'
        if (fnc == "logScale" || fnc == "log-scale")
        {
            Value arr = evaluate(expression[fnc]);
            validateArrayLength(arr, 2, 4,
                "Function 'log-scale' must have 2 to 4 values");
            double v = convertToDouble(arr[0]);
            double m = convertToDouble(arr[1]);
            double a = arr.size() > 2 ? convertToDouble(arr[2]) : 0;
            double b = arr.size() > 3 ? convertToDouble(arr[3]) : 100;
            v = std::min(v, m);
            double p = (b - a) / std::log(m + 1);
            return p * std::log(v + 1) + a;
        }

        // unknown
        if (Validating)
            THROW << "Unknown function <" << fnc << ">";
        return expression;
    }

    // string processing
    Value evaluateString(const std::string &s) const
    {
        if (Validating)
        {
            try
            {
                return evaluateStringInternal(s);
            }
            catch (...)
            {
                LOG(info3) << "In evaluation of string <"
                    << s << ">";
                throw;
            }
        }
        return evaluateStringInternal(s);
    }

    Value evaluateStringInternal(const std::string &s) const
    {
        // find '{'
        std::size_t start = s.find("{");
        if (start != s.npos)
        {
            size_t e = s.length();
            uint32 cnt = 1;
            std::size_t end = start;
            while (cnt > 0 && end + 1 < e)
            {
                end++;
                switch (s[end])
                {
                case '{': cnt++; break;
                case '}': cnt--; break;
                default: break;
                }
            }
            if (cnt > 0)
                THROW << "Missing '}' in <" << s << ">";
            if (end == start + 1)
                THROW << "Invalid '{}' in <" << s << ">";
            std::string subs = s.substr(start + 1, end - start - 1);
            Json::Value v;
            try
            {
                v = subs[0] == '{'
                    ? stringToJson(subs)
                    : Json::Value(subs);
            }
            catch (std::exception &e)
            {
                THROW << "Invalid json <" << subs
                    << ">, message <" << e.what() << ">";
            }
            subs = evaluate(v).asString();
            std::string res;
            if (start > 0)
                res += s.substr(0, start);
            res += subs;
            if (end < s.size())
                res += s.substr(end + 1);
            return evaluate(res);
        }
        // find '}'
        if (Validating)
        {
            auto end = s.find("}");
            if (end != s.npos)
                THROW << "Unmatched <}> in <" << s << ">";
        }
        // apply replacements
        return replacement(s);
    }

    // evaluation of expressions whose result is boolean
    bool filter(const Value &expression) const
    {
        if (Validating)
        {
            try
            {
                return filterInternal(expression);
            }
            catch (...)
            {
                LOG(info3) << "In filter <"
                    << expression.toStyledString() << ">";
                throw;
            }
        }
        return filterInternal(expression);
    }

    // some stylesheets contain an invalid aggregate filter
    //   in which the tests are all enclosed in additional array
    // this function tries to detect such cases and workaround it
    // example:
    //   invalid filter: ["all", [["has", "$foo"], ["has", "$bar"]]]
    //   correct filter: ["all", ["has", "$foo"], ["has", "$bar"]]
    const Value &aggregateFilterData(const Value &expression,
        uint32 &start) const
    {
        if (expression.size() == 2
            && expression[1].isArray()
            && expression[1][0].isArray())
        {
            start = 0;
            return expression[1];
        }
        else
        {
            start = 1;
            return expression;
        }
    }

    bool filterInternal(const Value &expression) const
    {
        if (Validating)
        {
            if (!expression.isArray())
                THROW << "Filter must be array.";
        }

        std::string cond = expression[0].asString();

        if (cond == "skip")
        {
            validateArrayLength(expression, 1, 1,
                "Invalid filter 'skip' array length.");
            return false;
        }

        // comparison filters
        if (cond == "==" || cond == "!=")
        {
            validateArrayLength(expression, 3, 3, std::string()
                + "Invalid filter '" + cond + "' array length.");
            Value a = evaluate(expression[1]);
            Value b = evaluate(expression[2]);
            bool op = cond == "==";
            if ((a.isString() || a.isNull()) && (b.isString() || b.isNull()))
                return (a.asString() == b.asString()) == op;
            return (convertToDouble(a) == convertToDouble(b)) == op;
        }
#define COMP(OP) \
if (cond == #OP) \
{ \
    validateArrayLength(expression, 3, 3, \
            "Invalid filter '" #OP "' array length."); \
    double a = convertToDouble(expression[1]); \
    double b = convertToDouble(expression[2]); \
    return a OP b; \
}
        COMP(>= );
        COMP(<= );
        COMP(> );
        COMP(< );
#undef COMP

        // negative filters
        if (!cond.empty() && cond[0] == '!')
        {
            Value v(expression);
            v[0] = cond.substr(1);
            return !filter(v);
        }

        // has filters
        if (cond == "has")
        {
            validateArrayLength(expression, 2, -1,
                "Invalid filter 'has' array length.");
            Value a = expression[1];
            return !replacement(a.asString()).empty();
        }

        // in filters
        if (cond == "in")
        {
            validateArrayLength(expression, 2, -1,
                "Invalid filter 'in' array length.");
            std::string v = evaluate(expression[1]).asString();
            for (uint32 i = 2, e = expression.size(); i < e; i++)
            {
                std::string m = evaluate(expression[i]).asString();
                if (v == m)
                    return true;
            }
            return false;
        }

        // aggregate filters
        if (cond == "all")
        {
            uint32 start;
            const Value &v = aggregateFilterData(expression, start);
            for (uint32 i = start, e = v.size(); i < e; i++)
                if (!filter(v[i]))
                    return false;
            return true;
        }
        if (cond == "any")
        {
            uint32 start;
            const Value &v = aggregateFilterData(expression, start);
            for (uint32 i = start, e = v.size(); i < e; i++)
                if (filter(v[i]))
                    return true;
            return false;
        }
        if (cond == "none")
        {
            uint32 start;
            const Value &v = aggregateFilterData(expression, start);
            for (uint32 i = start, e = v.size(); i < e; i++)
                if (filter(v[i]))
                    return false;
            return true;
        }

        // unknown filter
        if (Validating)
            THROW << "Unknown filter condition type.";

        return false;
    }

    void addFont(const std::string &name,
        std::vector<std::shared_ptr<void>> &output) const
    {
        auto it = stylesheet->fonts.find(name);
        if (it == stylesheet->fonts.end())
            THROW << "Could not find font <" << name << ">";
        output.push_back(it->second->info.userData);
    }

    void findFonts(const Value &expression,
        std::vector<std::shared_ptr<void>> &output) const
    {
        if (!expression.empty())
        {
            Value v = evaluate(expression);
            validateArrayLength(v, 1, -1, "Fonts must be an array");
            for (const Value &nj : v)
            {
                std::string n = nj.asString();
                addFont(n, output);
            }
        }
        addFont("#default", output);
    }

    // process single feature with specific style layer
    void processFeatureName(const std::string &layerName)
    {
        std::array<float, 2> tv;
        tv[0] = -std::numeric_limits<float>::infinity();
        tv[1] = +std::numeric_limits<float>::infinity();
        const Value &layer = style["layers"][layerName];
        if (Validating)
        {
            try
            {
                return processFeatureInternal(layer, tv, {});
            }
            catch (...)
            {
                LOG(info3)
                    << "In feature <" << feature->toStyledString()
                    << "> and layer name <" << layerName << ">";
                throw;
            }
        }
        return processFeatureInternal(layer, tv, {});
    }

    void processFeature(const Value &layer,
        std::array<float, 2> tileVisibility,
        boost::optional<sint32> zOverride)
    {
        if (Validating)
        {
            try
            {
                return processFeatureInternal(layer,
                    tileVisibility, zOverride);
            }
            catch (...)
            {
                LOG(info3) << "In layer <" << layer.toStyledString() << ">";
                throw;
            }
        }
        return processFeatureInternal(layer, tileVisibility, zOverride);
    }

    void processFeatureInternal(const Value &layer,
        std::array<float, 2> tileVisibility,
        boost::optional<sint32> zOverride)
    {
        currentLayer = &layer;
        AmpVarsScope ampVarsScope(ampVariables);

        // filter
        if (!zOverride && layer.isMember("filter")
            && !filter(layer["filter"]))
            return;

        // visible
        if (layer.isMember("visible")
            && !evaluate(layer["visible"]).asBool())
            return;

        // next-pass
        {
            auto np = layer["next-pass"];
            if (!np.empty())
            {
                std::string layerName = np[1].asString();
                if (Validating)
                {
                    if (!style["layers"].isMember(layerName))
                        THROW << "Invalid layer name <"
                        << layerName << "> in next-pass";
                }
                processFeature(style["layers"][layerName],
                    tileVisibility, np[0].asInt());
            }
        }

        // visibility-switch
        if (layer.isMember("visibility-switch"))
        {
            validateArrayLength(layer["visibility-switch"], 1, -1,
                "Visibility-switch must be an array.");
            float ve = -std::numeric_limits<float>::infinity();
            for (const Value &vs : layer["visibility-switch"])
            {
                validateArrayLength(vs, 2, 2, "All visibility-switch "
                    "elements must be arrays with 2 elements");
                float a = evaluate(vs[0]).asFloat();
                if (Validating)
                {
                    if (a <= ve)
                        THROW << "Values in visibility-switch "
                        "must be increasing";
                }
                if (!vs[1].empty())
                {
                    std::array<float, 2> tv;
                    tv[0] = std::max(tileVisibility[0], ve);
                    tv[1] = std::min(tileVisibility[1], a);
                    if (tv[0] < tv[1])
                    {
                        std::string ln = evaluate(vs[1]).asString();
                        if (Validating)
                        {
                            if (!style["layers"].isMember(ln))
                                THROW << "Invalid layer name <"
                                << ln << "> in visibility-switch";
                        }
                        Value l = layer;
                        l.removeMember("filter");
                        l.removeMember("visible");
                        l.removeMember("next-pass");
                        l.removeMember("visibility-switch");
                        const Value &s = style["layers"][ln];
                        for (auto n : s.getMemberNames())
                            l[n] = s[n];
                        l["filter"] = layer["filter"];
                        processFeature(l, tv, zOverride);
                    }
                }
                ve = a;
            }
            return;
        }

        GpuGeodataSpec spec;
        spec.commonData.tileVisibility[0] = tileVisibility[0];
        spec.commonData.tileVisibility[1] = tileVisibility[1];
        processFeatureCommon(layer, spec, zOverride);

        // point
        if (evaluate(layer["point"]).asBool())
            processFeaturePoint(layer, spec);

        // line
        if (evaluate(layer["line"]).asBool())
            processFeatureLine(layer, spec);

        // icon
        if (evaluate(layer["icon"]).asBool())
            processFeatureIcon(layer, spec);

        // label flat
        if (evaluate(layer["line-label"]).asBool())
            processFeatureLabelFlat(layer, spec);

        // label screen
        if (evaluate(layer["label"]).asBool())
            processFeatureLabelScreen(layer, spec);

        // polygon
        if (evaluate(layer["polygon"]).asBool())
            processFeaturePolygon(layer, spec);
    }

    void addIconSpec(const Value &layer, GpuGeodataSpec &spec) const
    {
        Value src = evaluate(layer["icon-source"]);
        validateArrayLength(src, 5, 5, "icon-source must have 5 values");
        std::string btm = src[0].asString();
        if (stylesheet->bitmaps.count(btm) != 1)
        {
            if (Validating)
                THROW << "invalid bitmap name <" << btm << ">";
            return;
        }
        auto tex = stylesheet->bitmaps.at(btm);
        assert(stylesheet->map->getResourceValidity(tex)
            == Validity::Valid);
        spec.bitmap = tex->info.userData;

        spec.commonData.icon.scale
            = layer.isMember("icon-scale")
            ? convertToDouble(layer["icon-scale"])
            : 1;
        if (compatibility)
            spec.commonData.icon.scale *= 0.5;

        spec.commonData.icon.origin
            = layer.isMember("icon-origin")
            ? convertOrigin(layer["icon-origin"])
            : GpuGeodataSpec::Origin::BottomCenter;

        vecToRaw(layer.isMember("icon-offset")
            ? convertVector2(layer["icon-offset"])
            : vec2f(0, 0),
            spec.commonData.icon.offset);
        spec.commonData.icon.offset[1] *= -1;

        if (layer.isMember("icon-no-overlap"))
            spec.commonData.preventOverlap
            = spec.commonData.preventOverlap
            && evaluate(layer["icon-no-overlap"]).asBool();

        vecToRaw(vec2f(5, 5), spec.commonData.icon.margin);
        if (layer.isMember("icon-no-overlap-margin"))
            vecToRaw(convertNoOverlapMargin(layer["icon-no-overlap-margin"]),
                spec.commonData.icon.margin);

        vecToRaw(layer.isMember("icon-color")
            ? convertColor(layer["icon-color"])
            : vec4f(1, 1, 1, 1),
            spec.commonData.icon.color);

        if (layer.isMember("icon-stick"))
            spec.commonData.stick = convertStick(layer["icon-stick"]);
    }

    void addIconItems(const Value &layer,
        GpuGeodataSpec &data, uint32 itemsCount)
    {
        if (!(data.commonData.icon.scale == data.commonData.icon.scale))
            return;
        Value src = evaluate(layer["icon-source"]);
        auto tex = stylesheet->bitmaps.at(src[0].asString());
        assert(tex);
        sint32 a[4] = { src[1].asInt(), src[2].asInt(),
            src[3].asInt(), src[4].asInt()};
        float s[2] = { tex->width - 1.f, tex->height - 1.f };
        std::array<float, 6> uv =
        {
            a[0] / s[0],
            1.f - (a[1] + a[3]) / s[1],
            (a[0] + a[2]) / s[0],
            1.f - a[1] / s[1],
            (float)a[2],
            (float)a[3]
        };
        data.iconCoords.reserve(data.iconCoords.size() + itemsCount);
        for (uint32 i = 0; i < itemsCount; i++)
            data.iconCoords.push_back(uv);
    }

    std::string addHysteresisIdSpec(const Value &layer,
        GpuGeodataSpec &spec)
    {
        std::string hysteresisId;
        if (layer.isMember("hysteresis"))
        {
            Value arr = evaluate(layer["hysteresis"]);
            validateArrayLength(arr, 4, 4,
                "hysteresis must have 4 values");
            spec.commonData.hysteresisDuration[0]
                = convertToDouble(arr[0]) / 1000.0;
            spec.commonData.hysteresisDuration[1]
                = convertToDouble(arr[1]) / 1000.0;
            hysteresisId = arr[2].asString();
            if (Validating && hysteresisId.empty())
                THROW << "Empty hysteresis id";
        }
        return hysteresisId;
    }

    void addHysteresisIdItems(const std::string &hysteresisId,
        GpuGeodataSpec &data, uint32 itemsCount)
    {
        if (hysteresisId.empty())
            return;
        data.hysteresisIds.reserve(data.hysteresisIds.size() + itemsCount);
        for (uint32 i = 0; i < itemsCount; i++)
            data.hysteresisIds.push_back(hysteresisId);
    }

    float addImportanceSpec(const Value &layer,
        GpuGeodataSpec &spec, float *overrideMargin = nullptr)
    {
        float importance = nan1();
        if (layer.isMember("importance-source"))
            importance = convertToDouble(
                evaluate(layer["importance-source"]));
        if (layer.isMember("importance-weight"))
            importance *= convertToDouble(
                evaluate(layer["importance-weight"]));
        if (importance == importance
            && browserOptions.isMember("mapFeaturesReduceMode")
            && browserOptions["mapFeaturesReduceMode"] == "scr-count7")
        {
            Value params = browserOptions["mapFeaturesReduceParams"];
            float dpi = data->map->options.pixelsPerInch;

            if (overrideMargin)
            {
                overrideMargin[0] = overrideMargin[1]
                    = convertToDouble(params[0]) * dpi;
            }

            spec.commonData.featuresLimitPerPixelSquared
                = convertToDouble(params[1]) / (dpi * dpi);

            spec.commonData.importanceDistanceFactor
                = convertToDouble(params[2]);

            if (params[3].asInt())
                spec.commonData.depthVisibilityThreshold
                = convertToDouble(params[4]);
        }
        return importance;
    }

    void addImportanceItems(float importance,
        GpuGeodataSpec &data, uint32 itemsCount)
    {
        if (!(importance == importance))
            return;
        data.importances.reserve(data.importances.size() + itemsCount);
        for (uint32 i = 0; i < itemsCount; i++)
            data.importances.push_back(importance);
    }

    void processFeatureCommon(const Value &layer, GpuGeodataSpec &spec,
        boost::optional<sint32> zOverride) const
    {
        // model matrix
        matToRaw(group->model, spec.model);

        // z-index
        if (zOverride)
            spec.commonData.zIndex = *zOverride;
        else if (layer.isMember("z-index"))
            spec.commonData.zIndex = evaluate(layer["z-index"]).asInt();

        // zbuffer-offset
        if (layer.isMember("zbuffer-offset"))
        {
            Value arr = evaluate(layer["zbuffer-offset"]);
            validateArrayLength(arr, 3, 3,
                "zbuffer-offset must have 3 values");
            for (int i = 0; i < 3; i++)
                spec.commonData.zBufferOffset[i] = arr[i].asFloat();
        }
        else
        {
            for (int i = 0; i < 3; i++)
                spec.commonData.zBufferOffset[i] = 0;
        }

        // visibility
        if (layer.isMember("visibility"))
        {
            spec.commonData.visibilities[0]
                = convertToDouble(layer["visibility"]);
        }

        // visibility-abs
        if (layer.isMember("visibility-abs"))
        {
            Value arr = evaluate(layer["visibility-abs"]);
            validateArrayLength(arr, 2, 2,
                "visibility-abs must have 2 values");
            for (int i = 0; i < 2; i++)
                spec.commonData.visibilities[i + 1]
                    = convertToDouble(arr[i]);
        }

        // visibility-rel
        if (layer.isMember("visibility-rel"))
        {
            Value arr = evaluate(layer["visibility-rel"]);
            validateArrayLength(arr, 4, 4,
                "visibility-rel must have 4 values");
            float d = convertToDouble(arr[0]) * convertToDouble(arr[1]);
            float v3 = convertToDouble(arr[3]);
            float v2 = convertToDouble(arr[2]);
            vec2f vr = vec2f(v3 <= 0 ? 0 : d / v3, v2 <= 0 ? 0 : d / v2);
            float *vs = spec.commonData.visibilities;
            if (vs[1] == vs[1])
            {
                // merge with visibility-abs
                vs[1] = std::max(vs[1], vr[0]);
                vs[2] = std::min(vs[2], vr[1]);
            }
            else
            {
                vs[1] = vr[0];
                vs[2] = vr[1];
            }
        }

        // culling
        if (layer.isMember("culling"))
            spec.commonData.visibilities[3]
                = convertToDouble(layer["culling"]);
    }

    void processFeaturePoint(const Value &layer, GpuGeodataSpec spec)
    {
        if (evaluate(layer["point-flat"]).asBool())
            spec.type = GpuGeodataSpec::Type::PointFlat;
        else
            spec.type = GpuGeodataSpec::Type::PointScreen;
        vecToRaw(layer.isMember("point-color")
            ? convertColor(layer["point-color"])
            : vec4f(1, 1, 1, 1),
            spec.unionData.point.color);
        spec.unionData.point.radius
            = layer.isMember("point-radius")
            ? convertToDouble(layer["point-radius"])
            : 1;
        if (compatibility)
            spec.unionData.point.radius *= 0.25;
        GpuGeodataSpec &data = findSpecData(spec);
        auto arr = getFeaturePositions();
        cullOutsideFeatures(arr);
        data.positions.reserve(data.positions.size() + arr.size());
        data.positions.insert(data.positions.end(), arr.begin(), arr.end());
    }

    void processFeatureLine(const Value &layer, GpuGeodataSpec spec)
    {
        if (evaluate(layer["line-flat"]).asBool())
            spec.type = GpuGeodataSpec::Type::LineFlat;
        else
            spec.type = GpuGeodataSpec::Type::LineScreen;
        vecToRaw(convertColor(layer["line-color"]),
            spec.unionData.line.color);
        spec.unionData.line.width
            = convertToDouble(layer["line-width"]);
        if (layer.isMember("line-width-units"))
        {
            std::string units = evaluate(layer["line-width-units"]).asString();
            if (units == "ratio")
                spec.unionData.line.units = GpuGeodataSpec::Units::Ratio;
            else if (units == "pixels")
                spec.unionData.line.units = GpuGeodataSpec::Units::Pixels;
            else if (units == "meters")
                spec.unionData.line.units = GpuGeodataSpec::Units::Meters;
            else if (Validating)
                THROW << "Invalid line-width-units";
            if (Validating)
            {
                if (spec.type == GpuGeodataSpec::Type::LineFlat
                    && spec.unionData.line.units
                    == GpuGeodataSpec::Units::Pixels)
                    THROW
                    << "Invalid combination of line-width-units"
                    << " (pixels) and line-flat (true)";
                if (spec.type == GpuGeodataSpec::Type::LineScreen
                    && spec.unionData.line.units
                    == GpuGeodataSpec::Units::Meters)
                    THROW
                    << "Invalid combination of line-width-units"
                    << " (meters) and line-flat (false)";
            }
        }
        else if (spec.type == GpuGeodataSpec::Type::LineFlat)
            spec.unionData.line.units = GpuGeodataSpec::Units::Meters;
        else
            spec.unionData.line.units = GpuGeodataSpec::Units::Pixels;
        GpuGeodataSpec &data = findSpecData(spec);
        const auto arr = getFeaturePositions();
        data.positions.reserve(data.positions.size() + arr.size());
        data.positions.insert(data.positions.end(), arr.begin(), arr.end());
    }

    void processFeatureIcon(const Value &layer, GpuGeodataSpec spec)
    {
        if (evaluate(layer["pack"]).asBool())
            return;

        spec.type = GpuGeodataSpec::Type::IconScreen;
        addIconSpec(layer, spec);

        std::string hysteresisId = addHysteresisIdSpec(layer, spec);

        float importance = addImportanceSpec(layer, spec,
            spec.commonData.icon.margin);

        GpuGeodataSpec &data = findSpecData(spec);
        const auto arr = getFeaturePositions();
        data.positions.reserve(data.positions.size() + arr.size());
        data.positions.insert(data.positions.end(), arr.begin(), arr.end());
        addHysteresisIdItems(hysteresisId, data, arr.size());
        addImportanceItems(importance, data, arr.size());
        addIconItems(layer, data, arr.size());
    }

    void processFeatureLabelFlat(const Value &layer, GpuGeodataSpec spec)
    {
        (void)layer;
        (void)spec;
        // todo
    }

    void processFeatureLabelScreen(const Value &layer, GpuGeodataSpec spec)
    {
        findFonts(layer["label-font"], spec.fontCascade);

        spec.type = GpuGeodataSpec::Type::LabelScreen;

        if (evaluate(layer["icon"]).asBool()
            && evaluate(layer["pack"]).asBool())
            addIconSpec(layer, spec);

        vecToRaw(layer.isMember("label-color")
            ? convertColor(layer["label-color"])
            : vec4f(1, 1, 1, 1),
            spec.unionData.labelScreen.color);
        vecToRaw(layer.isMember("label-color2")
            ? convertColor(layer["label-color2"])
            : vec4f(0, 0, 0, 1),
            spec.unionData.labelScreen.color2);

        vecToRaw(layer.isMember("label-outline")
            ? convertVector4(layer["label-outline"])
            : vec4f(0.25, 0.75, 2.2, 2.2),
            spec.unionData.labelScreen.outline);

        vecToRaw(layer.isMember("label-offset")
            ? convertVector2(layer["label-offset"])
            : vec2f(0, 0),
            spec.unionData.labelScreen.offset);
        spec.unionData.labelScreen.offset[1] *= -1;
        if (compatibility)
        {
            spec.unionData.labelScreen.offset[0] *= 0.5;
            spec.unionData.labelScreen.offset[1] *= 0.5;
        }

        if (layer.isMember("label-no-overlap"))
            spec.commonData.preventOverlap
                = spec.commonData.preventOverlap
                    && evaluate(layer["label-no-overlap"]).asBool();

        vecToRaw(vec2f(5, 5), spec.unionData.labelScreen.margin);
        if (layer.isMember("label-no-overlap-margin"))
            vecToRaw(convertNoOverlapMargin(layer["label-no-overlap-margin"]),
                spec.unionData.labelScreen.margin);

        spec.unionData.labelScreen.size
            = layer.isMember("label-size")
            ? convertToDouble(layer["label-size"])
            : 20;
        if (compatibility)
            spec.unionData.labelScreen.size *= 1.5 * 1.52 / 3.0;

        spec.unionData.labelScreen.width
            = layer.isMember("label-width")
            ? convertToDouble(layer["label-width"])
            : 200;

        spec.unionData.labelScreen.origin
            = layer.isMember("label-origin")
            ? convertOrigin(layer["label-origin"])
            : GpuGeodataSpec::Origin::BottomCenter;

        spec.unionData.labelScreen.textAlign
            = layer.isMember("label-align")
            ? convertTextAlign(layer["label-align"])
            : GpuGeodataSpec::TextAlign::Center;

        if (layer.isMember("label-stick"))
            spec.commonData.stick
            = convertStick(layer["label-stick"]);

        std::string text = evaluate(layer.isMember("label-source")
            ? layer["label-source"] : "$name").asString();
        if (text.empty())
            return;

        std::string hysteresisId = addHysteresisIdSpec(layer, spec);

        float importance = addImportanceSpec(layer, spec,
            spec.unionData.labelScreen.margin);

        GpuGeodataSpec &data = findSpecData(spec);
        auto arr = getFeaturePositions();
        cullOutsideFeatures(arr);
        data.positions.reserve(data.positions.size() + arr.size());
        data.positions.insert(data.positions.end(), arr.begin(), arr.end());
        data.texts.reserve(data.texts.size() + arr.size());
        for (uint32 i = 0, cnt = arr.size(); i < cnt; i++)
            data.texts.push_back(text);
        addHysteresisIdItems(hysteresisId, data, arr.size());
        addImportanceItems(importance, data, arr.size());
        addIconItems(layer, data, arr.size());
    }

    void processFeaturePolygon(const Value &layer, GpuGeodataSpec spec)
    {
        spec.type = GpuGeodataSpec::Type::Triangles;

        vecToRaw(layer.isMember("polygon-color")
            ? convertColor(layer["polygon-color"])
            : vec4f(1, 1, 1, 1),
            spec.unionData.triangles.color);

        if (layer.isMember("polygon-style"))
        {
            Value v = evaluate(layer["polygon-style"]);
            if (v.asString() == "solid")
                spec.unionData.triangles.style
                    = GpuGeodataSpec::PolygonStyle::Solid;
            else if (v.asString() == "flatshade")
                spec.unionData.triangles.style
                    = GpuGeodataSpec::PolygonStyle::FlatShade;
            else if (Validating)
                THROW << "Invalid polygon style";
        }
        else
        {
            spec.unionData.triangles.style
                = GpuGeodataSpec::PolygonStyle::FlatShade;
        }

        if (layer.isMember("polygon-use-stencil"))
        {
            Value v = evaluate(layer["polygon-use-stencil"]);
            spec.unionData.triangles.useStencil = v.asBool();
        }

        GpuGeodataSpec &data = findSpecData(spec);
        const auto arr = getFeatureTriangles();
        data.positions.reserve(data.positions.size() + arr.size());
        data.positions.insert(data.positions.end(), arr.begin(), arr.end());
    }

    GpuGeodataSpec &findSpecData(const GpuGeodataSpec &spec)
    {
        // only modifying attributes not used in comparison
        auto specIt = cacheData.find(spec);
        if (specIt == cacheData.end())
            specIt = cacheData.insert(spec).first;
        GpuGeodataSpec &data = const_cast<GpuGeodataSpec&>(*specIt);
        return data;
    }

    // immutable data
    //   data given from the outside world

    GeodataTile *const data;
    const GeodataStylesheet *const stylesheet;
    Value style;
    Value features;
    Value browserOptions;
    const vec3 aabbPhys[2];
    const uint32 lod;
    const bool compatibility;

    // processing data
    //   fast accessors to currently processed feature
    //   and the attached group, type and layer

    struct Group
    {
        const Value &group;

        Group(const Value &group) : group(group)
        {
            const Value &a = group["bbox"][0];
            vec3 aa = vec3(a[0].asDouble(), a[1].asDouble(), a[2].asDouble());
            const Value &b = group["bbox"][1];
            vec3 bb = vec3(b[0].asDouble(), b[1].asDouble(), b[2].asDouble());
            double resolution = group["resolution"].asDouble();
            vec3 mm = bb - aa;
            orthonormalize = mat4to3(scaleMatrix(
                mm / resolution)).cast<float>();
            model = translationMatrix(aa) * scaleMatrix(1);
        }

        Point convertPoint(const Value &v) const
        {
            validateArrayLength(v, 3, 3, "Point must have 3 coordinates");
            vec3f p = vec3f(v[0].asDouble(), v[1].asDouble(), v[2].asDouble());
            p = orthonormalize * p;
            return { p[0], p[1], p[2] };
        }

        std::vector<Point> convertArray(const Value &v, bool relative) const
        {
            (void)relative; // todo
            std::vector<Point> a;
            a.reserve(v.size());
            for (const Value &p : v)
                a.push_back(convertPoint(p));
            return a;
        }

        mat4 model;

    private:
        mat3f orthonormalize;
    };

    boost::optional<Group> group;
    boost::optional<Type> type;
    boost::optional<const Value &> feature;

    std::vector<std::vector<Point>> getFeaturePositions() const
    {
        std::vector<std::vector<Point>> result;
        switch (*type)
        {
        case Type::Point:
        {
            const Value &array1 = (*feature)["points"];
            result.push_back(
                group->convertArray(array1, false));
            // todo d-points
        } break;
        case Type::Line:
        {
            const Value &array1 = (*feature)["lines"];
            for (const Value &array2 : array1)
            {
                result.push_back(
                    group->convertArray(array2, false));
            }
            // todo d-lines
        } break;
        case Type::Polygon:
        {
            std::vector<Point> r;
            r.push_back(group->convertPoint((*feature)["middle"]));
            result.push_back(r);
        } break;
        }
        return result;
    }

    std::vector<std::vector<Point>> getFeatureTriangles() const
    {
        std::vector<Point> result;
        assert(*type == Type::Polygon);
        const Value &array1 = (*feature)["vertices"];
        if (Validating)
        {
            if (!array1.isArray() || (array1.size() % 3) != 0)
                THROW << "Polygon vertices must be an array "
                "with size divisible by 3";
        }
        std::vector<Point> vertices;
        vertices.reserve(array1.size());
        for (uint32 i = 0, e = array1.size(); i < e; i += 3)
        {
            Value v;
            v.resize(3);
            for (uint32 j = 0; j < 3; j++)
                v[j] = array1[i + j];
            vertices.push_back(group->convertPoint(v));
        }
        const Value &surface = (*feature)["surface"];
        if (Validating)
        {
            if (!surface.isArray() || (surface.size() % 3) != 0)
                THROW << "Polygon surface must be an array "
                         "with size divisible by 3";
        }
        auto verticesCount = vertices.size();
        for (const Value &vi : surface)
        {
            Json::UInt i = vi.asUInt();
            if (i >= verticesCount)
                THROW << "Index out of range (polygon surface vertex)";
            result.push_back(vertices[i]);
        }
        return { result };
    }

    void cullOutsideFeatures(std::vector<std::vector<Point>> &fps) const
    {
        for (auto &v : fps)
        {
            v.erase(std::remove_if(v.begin(), v.end(),
                [&](const Point &p) {
                vec3 a = vec4to3(vec4(group->model
                    * vec3to4(rawToVec3(p.data()).cast<double>(), 1.0)));
                for (int i = 0; i < 3; i++)
                {
                    if (a[i] < aabbPhys[0][i])
                        return true;
                    if (a[i] > aabbPhys[1][i])
                        return true;
                }
                return false;
            }), v.end());
        }
        fps.erase(std::remove_if(fps.begin(), fps.end(),
            [&](const std::vector<Point> &v) {
            return v.empty();
        }), fps.end());
    }

    // cache data
    //   temporary data generated while processing features

    std::set<GpuGeodataSpec, GpuGeodataSpecComparator> cacheData;
    AmpVariables ampVariables;
    const Value *currentLayer;
};

} // namespace

void GeodataTile::load()
{
    LOG(info2) << "Loading (gpu) geodata <" << name << ">";

    // this resource is not meant to be downloaded
    assert(!fetch);

    // upload
    renders.clear();
    renders.reserve(specsToUpload.size());
    uint32 index = 0;
    for (auto &spec : specsToUpload)
    {
        RenderGeodataTask t;
        t.geodata = std::make_shared<GpuGeodata>();
        std::stringstream ss;
        ss << name << "#$!" << index++;
        map->callbacks.loadGeodata(t.geodata->info, spec, ss.str());
        renders.push_back(t);
    }
    std::vector<GpuGeodataSpec>().swap(specsToUpload);

    // memory consumption
    info.ramMemoryCost = sizeof(*this)
        + renders.size() * sizeof(RenderGeodataTask);
    for (const RenderGeodataTask &it : renders)
    {
        info.gpuMemoryCost += it.geodata->info.gpuMemoryCost;
        info.ramMemoryCost += it.geodata->info.ramMemoryCost;
    }
}

void GeodataTile::process()
{
    OPTICK_EVENT();
    LOG(info2) << "Processing geodata <" << name << ">";

    if (map->options.debugValidateGeodataStyles)
    {
        geoContext<true> ctx(this);
        ctx.process();
    }
    else
    {
        geoContext<false> ctx(this);
        ctx.process();
    }

    state = Resource::State::downloaded;
    map->resources.queUpload.push(shared_from_this());
}

void MapImpl::resourcesGeodataProcessorEntry()
    {
    OPTICK_THREAD("geodata_processor");
        setLogThreadName("geodata processor");
        while (!resources.queGeodata.stopped())
        {
            std::weak_ptr<GeodataTile> w;
            resources.queGeodata.waitPop(w);
            std::shared_ptr<GeodataTile> r = w.lock();
            if (!r)
                continue;
            try
            {
                r->process();
            }
            catch (const std::exception &)
            {
                r->state = Resource::State::errorFatal;
            }
        }
    }

} // namespace vts
