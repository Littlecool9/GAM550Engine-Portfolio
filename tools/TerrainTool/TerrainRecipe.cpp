/**
 * @file TerrainRecipe.cpp
 * @author Jackey Cen
 * @brief Terrain source recipe: JSON validation, canonical text and source fingerprint.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainRecipe.h"

#include "TerrainNoise.h"

#include <assets/AssetId.h>
#include <core/fs/FileIO.h>
#include <terrain/TileHeights.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>

namespace mir::terrainbake
{
    namespace
    {
        using Json = nlohmann::json;

        constexpr double kMaxAmplitude = 10000.0;
        constexpr double kMinWavelength = 0.01;
        constexpr double kMaxWavelength = 1.0e6;
        constexpr double kMaxTilt = 1.0;
        constexpr double kMaxPlainsBlend = 1000.0;

        std::string JoinPath(std::string_view parent, std::string_view key)
        {
            std::string path(parent);
            if (!path.empty())
                path += '.';
            path += key;
            return path;
        }

        std::string FormatNumber(double value)
        {
            if (std::isnan(value))
                return "nan";
            if (std::isinf(value))
                return value < 0.0 ? "-inf" : "inf";
            char buffer[64];
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
            return std::string(buffer, result.ptr);
        }

        // Floats keep a ".0" so 1.0 does not read like an integer in messages.
        std::string Describe(const Json &value)
        {
            if (value.is_number_float())
            {
                std::string text = FormatNumber(value.get<double>());
                if (text.find_first_of(".eni") == std::string::npos)
                    text += ".0";
                return text;
            }
            std::string text = value.dump();
            constexpr size_t kMaxLength = 64;
            if (text.size() > kMaxLength)
                text = text.substr(0, kMaxLength) + "...";
            return text;
        }

        class Checker
        {
        public:
            void Add(std::string field, std::string message) { mIssues.push_back({std::move(field), std::move(message)}); }
            bool Clean() const { return mIssues.empty(); }
            std::vector<RecipeIssue> Take() { return std::move(mIssues); }

        private:
            std::vector<RecipeIssue> mIssues;
        };

        // nlohmann silently keeps the last repeated key; this reports them.
        class DuplicateKeyTracker
        {
        public:
            explicit DuplicateKeyTracker(Checker &checker) : mChecker(checker) {}

            std::string CurrentPath() const
            {
                std::string path;
                for (const Level &level : mLevels)
                    path = JoinPath(path, level.key.empty() ? "[]" : level.key);
                return path;
            }

            bool OnEvent(Json::parse_event_t event, const Json &parsed)
            {
                switch (event)
                {
                case Json::parse_event_t::object_start:
                case Json::parse_event_t::array_start:
                    mLevels.emplace_back();
                    break;
                case Json::parse_event_t::object_end:
                case Json::parse_event_t::array_end:
                    mLevels.pop_back();
                    break;
                case Json::parse_event_t::key:
                {
                    Level &level = mLevels.back();
                    level.key = parsed.get<std::string>();
                    if (!level.seen.insert(level.key).second)
                        mChecker.Add(CurrentPath(), "duplicate field; only one value per key is allowed");
                    break;
                }
                default:
                    break;
                }
                return true;
            }

        private:
            struct Level
            {
                std::string key;
                std::set<std::string> seen;
            };

            Checker &mChecker;
            std::vector<Level> mLevels;
        };

        // A missing or mistyped object is reported once; its fields then read as absent.
        class ObjectReader
        {
        public:
            ObjectReader(Checker &checker, const Json *object, std::string path,
                         std::initializer_list<std::string_view> keys)
                : mChecker(checker), mPath(std::move(path))
            {
                if (!object)
                    return;
                if (!object->is_object())
                {
                    mChecker.Add(mPath, "expected an object, got " + Describe(*object));
                    return;
                }
                mObject = object;
                for (const auto &[key, value] : object->items())
                    if (std::find(keys.begin(), keys.end(), key) == keys.end())
                        mChecker.Add(Path(key), "unknown field (value " + Describe(value) + ")");
            }

            std::string Path(std::string_view key) const { return JoinPath(mPath, key); }

            const Json *Field(std::string_view key) const
            {
                if (!mObject)
                    return nullptr;
                const auto it = mObject->find(key);
                if (it == mObject->end())
                {
                    mChecker.Add(Path(key), "missing required field");
                    return nullptr;
                }
                return &*it;
            }

            ObjectReader Object(std::string_view key, std::initializer_list<std::string_view> keys) const
            {
                return ObjectReader(mChecker, Field(key), Path(key), keys);
            }

            // An integer must convert exactly; -0 reads as +0 so it cannot split a fingerprint.
            std::optional<double> FiniteNumber(std::string_view key) const
            {
                const Json *value = Field(key);
                if (!value)
                    return std::nullopt;
                double number = 0.0;
                if (value->is_number_float())
                {
                    number = value->get<double>();
                    if (!std::isfinite(number))
                    {
                        mChecker.Add(Path(key), Describe(*value) + " is not a finite number");
                        return std::nullopt;
                    }
                }
                else if (value->is_number_unsigned())
                {
                    const uint64_t integer = value->get<uint64_t>();
                    number = static_cast<double>(integer);
                    if (number >= 18446744073709551616.0 || static_cast<uint64_t>(number) != integer)
                    {
                        mChecker.Add(Path(key), Describe(*value) + " is not exactly representable as a double");
                        return std::nullopt;
                    }
                }
                else if (value->is_number_integer())
                {
                    const int64_t integer = value->get<int64_t>();
                    number = static_cast<double>(integer);
                    if (number < -9223372036854775808.0 || static_cast<int64_t>(number) != integer)
                    {
                        mChecker.Add(Path(key), Describe(*value) + " is not exactly representable as a double");
                        return std::nullopt;
                    }
                }
                else
                {
                    mChecker.Add(Path(key), "expected a number, got " + Describe(*value));
                    return std::nullopt;
                }
                return number == 0.0 ? 0.0 : number;
            }

            std::optional<double> Number(std::string_view key, double low, double high, std::string_view unit) const
            {
                const std::optional<double> number = FiniteNumber(key);
                if (number && (*number < low || *number > high))
                {
                    mChecker.Add(Path(key), FormatNumber(*number) + " is out of range [" + FormatNumber(low) + ", " +
                                                FormatNumber(high) + "]" + (unit.empty() ? "" : " ") +
                                                std::string(unit));
                    return std::nullopt;
                }
                return number;
            }

            std::optional<int64_t> Integer(std::string_view key, int64_t low, int64_t high) const
            {
                const Json *value = Field(key);
                if (!value)
                    return std::nullopt;
                if (!value->is_number_integer())
                {
                    mChecker.Add(Path(key), "expected an integer, got " + Describe(*value));
                    return std::nullopt;
                }
                // Integers past int64 only fit the unsigned type and are out of any range used here.
                const bool fits = !value->is_number_unsigned() ||
                                  value->get<uint64_t>() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
                const int64_t integer = fits ? value->get<int64_t>() : std::numeric_limits<int64_t>::max();
                if (!fits || integer < low || integer > high)
                {
                    mChecker.Add(Path(key), Describe(*value) + " is out of range [" + std::to_string(low) + ", " +
                                                std::to_string(high) + "]");
                    return std::nullopt;
                }
                return integer;
            }

            std::optional<uint64_t> Unsigned(std::string_view key) const
            {
                const Json *value = Field(key);
                if (!value)
                    return std::nullopt;
                if (!value->is_number_unsigned())
                {
                    mChecker.Add(Path(key), "expected an unsigned 64-bit integer, got " + Describe(*value));
                    return std::nullopt;
                }
                return value->get<uint64_t>();
            }

            std::optional<std::string> String(std::string_view key) const
            {
                const Json *value = Field(key);
                if (!value)
                    return std::nullopt;
                if (!value->is_string())
                {
                    mChecker.Add(Path(key), "expected a string, got " + Describe(*value));
                    return std::nullopt;
                }
                return value->get<std::string>();
            }

        private:
            Checker &mChecker;
            const Json *mObject = nullptr;
            std::string mPath;
        };

        struct TermResult
        {
            std::optional<double> amplitude;
            std::optional<double> wavelength;
            std::optional<int64_t> octaves;

            bool Valid() const { return amplitude && wavelength && octaves; }
            NoiseTerm Term() const { return {*amplitude, *wavelength, static_cast<int32_t>(*octaves)}; }
        };

        TermResult ReadTerm(const ObjectReader &parameters, std::string_view name)
        {
            const ObjectReader term = parameters.Object(name, {"amplitude", "wavelength", "octaves"});
            return {term.Number("amplitude", 0.0, kMaxAmplitude, "m"),
                    term.Number("wavelength", kMinWavelength, kMaxWavelength, "m"),
                    term.Integer("octaves", 1, kMaxNoiseOctaves)};
        }

        // Octaves finer than two samples alias into noise the grid cannot hold.
        void CheckResolvable(Checker &checker, const std::string &path, const NoiseTerm &term, double spacing)
        {
            const double finest = term.wavelength / static_cast<double>(uint64_t(1) << (term.octaves - 1));
            if (finest < 2.0 * spacing)
                checker.Add(path, "finest octave wavelength " + FormatNumber(finest) +
                                      " m (wavelength / 2^(octaves - 1)) is below 2 x sampleSpacing = " +
                                      FormatNumber(2.0 * spacing) + " m");
        }

        // Padding included.
        double SampleReach(const TerrainLayout &layout, bool zAxis)
        {
            const TileRange tiles = layout.Tiles();
            const int32_t first = zAxis ? tiles.min.z : tiles.min.x;
            const int32_t last = zAxis ? tiles.max.z : tiles.max.x;
            const double low = layout.SamplePosition(layout.TileFirstSample(first));
            const double high = layout.SamplePosition(layout.TileFirstSample(last) + layout.IntervalsPerTile());
            return std::max(std::abs(low), std::abs(high));
        }

        std::string Bits(double value)
        {
            const uint64_t bits = std::bit_cast<uint64_t>(value == 0.0 ? 0.0 : value);
            constexpr char kHex[] = "0123456789abcdef";
            std::string text = "f64:";
            for (int shift = 60; shift >= 0; shift -= 4)
                text += kHex[(bits >> shift) & 0xF];
            return text;
        }
    }

    RecipeParseResult ParseTerrainRecipe(std::string_view text)
    {
        Checker checker;
        Json root;
        DuplicateKeyTracker duplicates(checker);
        try
        {
            root = Json::parse(text, [&duplicates](int, Json::parse_event_t event, Json &parsed)
                               { return duplicates.OnEvent(event, parsed); });
        }
        catch (const Json::exception &error)
        {
            // Includes number overflow (1e400), which nlohmann reports as out_of_range.
            checker.Add(duplicates.CurrentPath(), std::string("not valid JSON: ") + error.what());
            return {std::nullopt, checker.Take()};
        }

        const ObjectReader top(checker, &root, "", {"recipeVersion", "generator", "seed", "layout", "parameters"});

        const std::optional<int64_t> recipeVersion = top.Integer("recipeVersion", 0, std::numeric_limits<uint32_t>::max());
        if (recipeVersion && *recipeVersion != kTerrainRecipeVersion)
            checker.Add("recipeVersion", "unsupported recipe version " + std::to_string(*recipeVersion) +
                                             " (supported: " + std::to_string(kTerrainRecipeVersion) + ")");

        const ObjectReader generator = top.Object("generator", {"id", "version"});
        const std::optional<std::string> generatorId = generator.String("id");
        const std::optional<int64_t> generatorVersion =
            generator.Integer("version", 0, std::numeric_limits<uint32_t>::max());
        bool generatorSupported = false;
        if (generatorId && *generatorId != kReliefGeneratorId)
            checker.Add("generator.id", "unsupported generator \"" + *generatorId + "\" (supported: \"" +
                                            std::string(kReliefGeneratorId) + "\")");
        else if (generatorId && generatorVersion && *generatorVersion != kReliefGeneratorVersion)
            checker.Add("generator.version", "unsupported version " + std::to_string(*generatorVersion) + " of \"" +
                                                  std::string(kReliefGeneratorId) + "\" (supported: " +
                                                  std::to_string(kReliefGeneratorVersion) + ")");
        else
            generatorSupported = generatorId && generatorVersion;

        const std::optional<uint64_t> seed = top.Unsigned("seed");

        const ObjectReader layoutReader = top.Object("layout", {"sampleSpacing", "intervalsPerTile", "bounds"});
        std::optional<double> spacing = layoutReader.FiniteNumber("sampleSpacing");
        if (spacing && *spacing <= 0.0)
        {
            checker.Add("layout.sampleSpacing", FormatNumber(*spacing) + " m must be greater than 0");
            spacing.reset();
        }
        const std::optional<int64_t> intervals =
            layoutReader.Integer("intervalsPerTile", 1, kMaxTerrainTileIntervals);
        const ObjectReader boundsReader = layoutReader.Object("bounds", {"minX", "minZ", "maxX", "maxZ"});
        const std::optional<double> minX = boundsReader.FiniteNumber("minX");
        const std::optional<double> minZ = boundsReader.FiniteNumber("minZ");
        const std::optional<double> maxX = boundsReader.FiniteNumber("maxX");
        const std::optional<double> maxZ = boundsReader.FiniteNumber("maxZ");
        bool boundsValid = minX && minZ && maxX && maxZ;
        if (minX && maxX && !(*minX < *maxX))
        {
            checker.Add("layout.bounds", "minX " + FormatNumber(*minX) + " must be less than maxX " + FormatNumber(*maxX));
            boundsValid = false;
        }
        if (minZ && maxZ && !(*minZ < *maxZ))
        {
            checker.Add("layout.bounds", "minZ " + FormatNumber(*minZ) + " must be less than maxZ " + FormatNumber(*maxZ));
            boundsValid = false;
        }
        std::optional<TerrainLayout> layout;
        if (spacing && intervals && boundsValid)
        {
            layout = TerrainLayout::Create(*spacing, static_cast<int32_t>(*intervals), {*minX, *minZ, *maxX, *maxZ});
            if (!layout)
                checker.Add("layout", "refused by TerrainLayout::Create: sample indices reach 2^53, tile indices "
                                      "leave int32, or the spacing is below 2^16 ulps of the largest coordinate");
        }

        // An unknown generator has no schema for its parameters.
        std::optional<ReliefParameters> relief;
        if (!generatorSupported)
            top.Field("parameters");
        else
        {
            const ObjectReader parameters =
                top.Object("parameters", {"baseHeight", "tilt", "broad", "ridges", "detail", "plains"});
            const std::optional<double> baseHeight =
                parameters.Number("baseHeight", -kMaxHeightMagnitude, kMaxHeightMagnitude, "m");
            const ObjectReader tilt = parameters.Object("tilt", {"x", "z"});
            const std::optional<double> tiltX = tilt.Number("x", -kMaxTilt, kMaxTilt, "m/m");
            const std::optional<double> tiltZ = tilt.Number("z", -kMaxTilt, kMaxTilt, "m/m");
            const TermResult broad = ReadTerm(parameters, "broad");
            const TermResult ridges = ReadTerm(parameters, "ridges");
            const TermResult detail = ReadTerm(parameters, "detail");
            const ObjectReader plains = parameters.Object("plains", {"height", "blend"});
            const std::optional<double> plainsHeight =
                plains.Number("height", -kMaxHeightMagnitude, kMaxHeightMagnitude, "m");
            const std::optional<double> plainsBlend = plains.Number("blend", 0.0, kMaxPlainsBlend, "m");

            if (spacing)
            {
                if (broad.Valid())
                    CheckResolvable(checker, "parameters.broad", broad.Term(), *spacing);
                if (ridges.Valid())
                    CheckResolvable(checker, "parameters.ridges", ridges.Term(), *spacing);
                if (detail.Valid())
                    CheckResolvable(checker, "parameters.detail", detail.Term(), *spacing);
            }

            if (baseHeight && tiltX && tiltZ && broad.Valid() && ridges.Valid() && detail.Valid() && plainsHeight &&
                plainsBlend)
            {
                relief = ReliefParameters{*baseHeight,     *tiltX,         *tiltZ,      broad.Term(),
                                          ridges.Term(),   detail.Term(),  *plainsHeight, *plainsBlend};
                if (layout)
                {
                    // The smooth floor adds at most blend / 4 above max(h, plains.height).
                    const double reach = std::abs(*baseHeight) + std::abs(*tiltX) * SampleReach(*layout, false) +
                                         std::abs(*tiltZ) * SampleReach(*layout, true) + broad.amplitude.value() +
                                         ridges.amplitude.value() + detail.amplitude.value();
                    const double bound = std::max(reach, std::abs(*plainsHeight)) + *plainsBlend * 0.25;
                    if (bound > kMaxHeightMagnitude)
                        checker.Add("parameters", "heights can reach +-" + FormatNumber(bound) +
                                                      " m over the map's tiles, beyond +-" +
                                                      FormatNumber(kMaxHeightMagnitude) + " m");
                }
            }
        }

        if (!checker.Clean() || !layout || !relief || !seed)
            return {std::nullopt, checker.Take()};
        return {TerrainRecipe{kTerrainRecipeVersion, *generatorId, static_cast<uint32_t>(*generatorVersion), *seed,
                              *layout, *relief},
                {}};
    }

    RecipeParseResult LoadTerrainRecipe(const std::filesystem::path &path)
    {
        const std::optional<std::string> text = ReadText(path);
        if (!text)
            return {std::nullopt, {{"", "cannot read " + path.generic_string()}}};
        return ParseTerrainRecipe(*text);
    }

    std::string CanonicalRecipeText(const TerrainRecipe &recipe)
    {
        const TerrainLayout &layout = recipe.layout;
        const ReliefParameters &relief = recipe.relief;
        std::map<std::string, std::string> fields;
        fields["canonicalVersion"] = std::to_string(kRecipeCanonicalVersion);
        fields["recipeVersion"] = std::to_string(recipe.recipeVersion);
        fields["generator.id"] = recipe.generatorId;
        fields["generator.version"] = std::to_string(recipe.generatorVersion);
        fields["seed"] = std::to_string(recipe.seed);
        fields["layout.sampleSpacing"] = Bits(layout.SampleSpacing());
        fields["layout.intervalsPerTile"] = std::to_string(layout.IntervalsPerTile());
        fields["layout.bounds.minX"] = Bits(layout.Bounds().minX);
        fields["layout.bounds.minZ"] = Bits(layout.Bounds().minZ);
        fields["layout.bounds.maxX"] = Bits(layout.Bounds().maxX);
        fields["layout.bounds.maxZ"] = Bits(layout.Bounds().maxZ);
        fields["parameters.baseHeight"] = Bits(relief.baseHeight);
        fields["parameters.tilt.x"] = Bits(relief.tiltX);
        fields["parameters.tilt.z"] = Bits(relief.tiltZ);
        const std::pair<const char *, const NoiseTerm *> terms[] = {
            {"broad", &relief.broad}, {"ridges", &relief.ridges}, {"detail", &relief.detail}};
        for (const auto &[name, term] : terms)
        {
            const std::string prefix = std::string("parameters.") + name + ".";
            fields[prefix + "amplitude"] = Bits(term->amplitude);
            fields[prefix + "wavelength"] = Bits(term->wavelength);
            fields[prefix + "octaves"] = std::to_string(term->octaves);
        }
        fields["parameters.plains.height"] = Bits(relief.plainsHeight);
        fields["parameters.plains.blend"] = Bits(relief.plainsBlend);

        std::string text;
        for (const auto &[key, value] : fields)
            text += key + '=' + value + '\n';
        return text;
    }

    TerrainSourceFingerprint FingerprintRecipe(const TerrainRecipe &recipe)
    {
        const uint64_t hash = Fnv1a64(CanonicalRecipeText(recipe));
        // Zero means "unknown"; a recipe hashing to it takes 1 instead.
        return TerrainSourceFingerprint(hash == 0 ? 1 : hash);
    }
}
