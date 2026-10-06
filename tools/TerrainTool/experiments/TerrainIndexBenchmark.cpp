#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRelease.h>
#include "../../../modules/terrain/src/terrain/LittleEndian.h"

#include <assets/AssetId.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <vector>

namespace
{
    std::vector<std::byte> MakeIndex(int mode, size_t &count)
    {
        using namespace mir;
        constexpr int side = 586;
        const TerrainLocalBounds bounds{0, 0, side * 256.0, side * 256.0};
        std::vector<TerrainLayout> layers;
        for (double spacing : {1.0, 2.0, 4.0, 8.0})
            if (mode != 0 || spacing == 8.0)
                layers.push_back(*TerrainLayout::CreateFromTileSize(256, spacing, bounds));
        std::map<std::pair<int, int>, uint32_t> chunkIds;
        std::vector<TerrainPackRecord> packs;
        for (int z = 0; z <= (side - 1) / 16; ++z)
            for (int x = 0; x <= (side - 1) / 16; ++x)
            {
                chunkIds[{z, x}] = static_cast<uint32_t>(packs.size());
                packs.push_back({{x, z}, 0, kTerrainPackHeaderSize, 0});
            }
        std::vector<TerrainTileRecord> records;
        records.reserve(size_t(side) * side * (mode == 2 ? 4 : 2));
        for (int z = 0; z < side; ++z)
            for (int x = 0; x < side; ++x)
            {
                const bool fine = mode == 2 || (mode == 1 && (z * side + x) % 10 == 0);
                for (uint32_t l = mode == 0 || fine ? 0 : 3; l < layers.size(); ++l)
                {
                    TerrainTileRecord r;
                    r.tile = {x, z}; r.layer = l; r.source = l == (mode == 0 || fine ? 0 : 3);
                    r.pack = chunkIds.at({z / 16, x / 16});
                    auto &pack = packs[r.pack];
                    r.offset = (pack.fileSize + 15) / 16 * 16;
                    r.samplesPerSide = layers[l].IntervalsPerTile() + 1;
                    r.length = uint64_t(r.samplesPerSide) * r.samplesPerSide * 4;
                    r.sampleSpacing = layers[l].SampleSpacing();
                    r.validExtent = *layers[l].ValidExtent(r.tile);
                    ++pack.tileCount; pack.fileSize = r.offset + r.length;
                    records.push_back(r);
                }
            }
        count = records.size();
        std::vector<std::byte> bytes;
        bytes.reserve(kTerrainIndexHeaderSize + packs.size() * kTerrainPackRecordSize + count * kTerrainTileRecordSize + 72);
        detail::LeWriter out(bytes);
        out.U32(0x49544a4d); out.U32(kTerrainFormatVersion.value); out.U32(0); out.U32(kTerrainIndexHeaderSize);
        out.U32(kTerrainHeightsContentVersion); out.U32(16);
        out.U64(0x145); out.U64(0x145); out.U64(0x145);
        out.F64(layers.front().SampleSpacing()); out.I32(layers.front().IntervalsPerTile()); out.I32(16);
        out.F64(bounds.minX); out.F64(bounds.minZ); out.F64(bounds.maxX); out.F64(bounds.maxZ);
        out.U32(static_cast<uint32_t>(packs.size())); out.U32(kTerrainPackRecordSize);
        out.U32(static_cast<uint32_t>(records.size())); out.U32(kTerrainTileRecordSize);
        out.U64(TerrainRecordsHeightsHash(records)); out.U32(static_cast<uint32_t>(layers.size())); out.U32(kTerrainLayerRecordSize);
        for (const auto &p : packs)
        {
            out.I32(p.chunk.x); out.I32(p.chunk.z); out.U32(p.tileCount); out.U32(0);
            out.U64(p.fileSize); out.U64(p.checksum);
        }
        for (const auto &r : records)
        {
            out.I32(r.tile.x); out.I32(r.tile.z); out.U32(r.pack); out.I32(r.samplesPerSide);
            out.U64(r.offset); out.U64(r.length); out.U64(r.checksum); out.F64(r.sampleSpacing);
            out.F32(r.minHeight); out.F32(r.maxHeight);
            out.F64(r.validExtent.minX); out.F64(r.validExtent.minZ); out.F64(r.validExtent.maxX); out.F64(r.validExtent.maxZ);
            out.U32(r.layer); out.U32(0); out.U32(0); out.U32(r.source ? 1 : 0);
            out.U32(0); out.U32(0); out.F64(0);
        }
        for (const auto &l : layers) { out.F64(l.SampleSpacing()); out.I32(l.IntervalsPerTile()); out.U32(0); }
        out.U64(Fnv1a64(bytes));
        return bytes;
    }
}

int main()
{
    std::cout << "scenario,tiles,layers,payloads,index_bytes,open_mean_ms,open_min_ms,open_median_ms\n";
    for (int mode = 0; mode < 3; ++mode)
    {
        size_t records = 0;
        const auto bytes = MakeIndex(mode, records);
        auto source = std::make_shared<mir::TerrainMemorySource>();
        source->Put(std::string(mir::kTerrainIndexFileName), bytes);
        double sum = 0, smallest = 1e30;
        std::vector<double> times;
        for (int repeat = 0; repeat < 3; ++repeat)
        {
            const auto start = std::chrono::steady_clock::now();
            const auto parsed = mir::TerrainReleaseHandle::Open(source);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (!parsed) { std::cerr << parsed.error.message << '\n'; return 1; }
            sum += ms; smallest = std::min(smallest, ms); times.push_back(ms);
        }
        std::sort(times.begin(), times.end());
        std::cout << (mode == 0 ? "coarse" : mode == 1 ? "fine_10pct" : "dense_four")
                  << ",343396," << (mode == 0 ? 1 : 4) << "," << records << ',' << bytes.size() << ',' << sum / 3 << ',' << smallest << ',' << times[1] << '\n';
    }
}
