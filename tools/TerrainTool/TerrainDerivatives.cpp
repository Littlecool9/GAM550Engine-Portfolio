#include "TerrainDerivatives.h"
#include "TerrainFinalSurface.h"
#include <terrain/TileAddressing.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <tuple>
#include <format>
#include <glm/geometric.hpp>

namespace mir::terrainbake
{
    namespace
    {
        using FinalSurface = TerrainFinalSurface;
        TerrainLodSurface Mesh(const FinalSurface &final,const TileHeights &source,const TerrainLayout &layout,
                               uint32_t layer,uint32_t sourceLayer,bool ring)
        {
            TerrainLodSurface lod;lod.tile=source.Tile();lod.layer=layer;lod.level=layer-sourceLayer;
            lod.sourceSpacing=final.Layout(source).SampleSpacing();lod.spacing=layout.SampleSpacing();
            lod.sourceChecksum=TerrainFinalHeightChecksum(source);
            const auto &ref=final.recipe.layout;
            const int64_t firstX=ref.TileFirstSample(lod.tile.x),firstZ=ref.TileFirstSample(lod.tile.z);
            lod.origin=*TerrainLocalPosition::FromMeters({ref.SamplePosition(firstX),0,ref.SamplePosition(firstZ)});
            const int ratio=ref.IntervalsPerTile()/layout.IntervalsPerTile();
            const int total=ref.IntervalsPerTile(),n=layout.IntervalsPerTile();
            std::map<std::pair<int,int>,uint32_t> ids;
            auto vertex=[&](int ix,int iz){
                auto [it,inserted]=ids.try_emplace({iz,ix},static_cast<uint32_t>(lod.positions.size()));
                if(inserted)
                {
                    const double x=ref.SamplePosition(firstX+ix),z=ref.SamplePosition(firstZ+iz);
                    const bool edge=ix==0||iz==0||ix==total||iz==total;
                    const double h=ring&&edge?final.SharedHeight(x,z,source):final.Height(source,x,z);
                    lod.positions.emplace_back(float(x-lod.origin.Meters().x),float(h),float(z-lod.origin.Meters().z));
                }
                return it->second;
            };
            std::array<int,4> edgeRatios{ratio,ratio,ratio,ratio};
            if(ring)
            {
                edgeRatios.fill(ref.IntervalsPerTile()/source.IntervalsPerTile());
                const std::array<std::pair<int,int>,4> dirs{{{-1,0},{1,0},{0,-1},{0,1}}};
                for(int e=0;e<4;++e)
                {
                    int64_t tx=int64_t(lod.tile.x)+dirs[e].first,tz=int64_t(lod.tile.z)+dirs[e].second;
                    if(tx<INT32_MIN||tx>INT32_MAX||tz<INT32_MIN||tz>INT32_MAX)continue;
                    if(const auto *other=final.Tile({int32_t(tx),int32_t(tz)}))
                        edgeRatios[e]=std::min(edgeRatios[e],ref.IntervalsPerTile()/other->IntervalsPerTile());
                }
            }
            const auto extent=*ref.ValidExtent(lod.tile);
            for(int iz=0;iz<n;++iz)for(int ix=0;ix<n;++ix)
            {
                const int x0=ix*ratio,x1=(ix+1)*ratio,z0=iz*ratio,z1=(iz+1)*ratio;
                const double gx0=ref.SamplePosition(firstX+x0),gx1=ref.SamplePosition(firstX+x1);
                const double gz0=ref.SamplePosition(firstZ+z0),gz1=ref.SamplePosition(firstZ+z1);
                if(gx1<=extent.minX||gx0>=extent.maxX||gz1<=extent.minZ||gz0>=extent.maxZ)continue;
                uint32_t a=vertex(x0,z0),b=vertex(x1,z0),c=vertex(x1,z1),d=vertex(x0,z1);
                std::vector<std::array<uint32_t,3>> triangles{{a,d,c},{a,c,b}};
                auto split=[&](uint32_t start,uint32_t end,int e,int from,int to,bool alongZ,int fixed){
                    uint32_t previous=start;
                    for(int v=from+edgeRatios[e];v<to;v+=edgeRatios[e])
                    {
                        const uint32_t point=alongZ?vertex(fixed,v):vertex(v,fixed);
                        for(size_t t=0;t<triangles.size();++t)
                        {
                            auto tri=triangles[t];bool done=false;
                            for(int j=0;j<3;++j)
                            {
                                const uint32_t u=tri[j],w=tri[(j+1)%3],third=tri[(j+2)%3];
                                if((u==previous&&w==end)||(u==end&&w==previous))
                                {
                                    triangles[t]={u,point,third};triangles.push_back({point,w,third});done=true;break;
                                }
                            }
                            if(done)break;
                        }
                        previous=point;
                    }
                };
                if(ix==0)split(a,d,0,z0,z1,true,x0);
                if(ix==n-1)split(b,c,1,z0,z1,true,x1);
                if(iz==0)split(a,b,2,x0,x1,false,z0);
                if(iz==n-1)split(d,c,3,x0,x1,false,z1);
                for(auto tri:triangles)lod.indices.insert(lod.indices.end(),tri.begin(),tri.end());
            }
            lod.topologyHash=TerrainTopologyHash(lod.positions,lod.indices);
            return lod;
        }
    }
    std::optional<TerrainDerivatives> BakeTerrainDerivatives(const TerrainRecipe &recipe,
        std::span<const TileHeights> heights,std::string &error,const TerrainRegionSettings *regions)
    {
        TerrainDerivatives result;result.layers.push_back(recipe.layout);
        double maxSource=recipe.layout.SampleSpacing();
        FinalSurface final{recipe,{}};
        for(const auto &tile:heights)
        {
            const auto layout=recipe.SourceLayout(tile.Tile());
            if(!layout||layout->IntervalsPerTile()!=tile.IntervalsPerTile()||
                !std::all_of(tile.Samples().begin(),tile.Samples().end(),[](float h){return std::isfinite(h);}))
            {error="final source block has no matching finite assigned layout";return {};}
            if(!final.tiles.emplace(std::pair(tile.Tile().z,tile.Tile().x),&tile).second)
            {error="final source tile is duplicated";return {};}
            maxSource=std::max(maxSource,layout->SampleSpacing());
        }
        const double maxSpacing=std::max(maxSource,kTerrainInitialMaxLodSpacing);
        while(result.layers.back().IntervalsPerTile()%2==0 && result.layers.back().SampleSpacing()*2<=maxSpacing)
        {
            const auto &prev=result.layers.back();auto next=TerrainLayout::Create(prev.SampleSpacing()*2,prev.IntervalsPerTile()/2,prev.Bounds());
            if(!next||!TerrainLayersNest(prev,*next))break;
            result.layers.push_back(*next);
        }
        for(const auto &tile:heights)
        {
            const auto sourceLayout=final.Layout(tile);
            auto found=std::find_if(result.layers.begin(),result.layers.end(),[&](const auto &v){return v.SampleSpacing()==sourceLayout.SampleSpacing();});
            if(found==result.layers.end()){error="source grid is missing from derivative layers";return {};}
            const uint32_t sourceLayer=static_cast<uint32_t>(found-result.layers.begin());
            for(uint32_t layer=sourceLayer;layer<result.layers.size();++layer)
            {
                const auto &layout=result.layers[layer];
                if(layer!=sourceLayer&&layout.SampleSpacing()>kTerrainInitialMaxLodSpacing)break;
                auto mesh=Mesh(final,tile,layout,layer,sourceLayer,true);
                auto bound=TerrainLodDeviation(sourceLayout,tile,mesh);
                if(!bound||mesh.positions.size()>UINT32_MAX||mesh.indices.size()>UINT32_MAX)
                {
                    double maxX=-INFINITY,maxZ=-INFINITY,minX=INFINITY,minZ=INFINITY;
                    for(auto v:mesh.positions){minX=std::min(minX,double(v.x));minZ=std::min(minZ,double(v.z));maxX=std::max(maxX,double(v.x));maxZ=std::max(maxZ,double(v.z));}
                    const auto extent=*layout.ValidExtent(tile.Tile());const auto origin=mesh.origin.Meters();
                    error=std::format("tile ({}, {}) LOD {} m: actual float support [{},{}]..[{},{}], valid [{} ,{}]..[{},{}]; uncovered X/Z widths {} / {} m; topology/error cannot be enclosed",
                        tile.Tile().x,tile.Tile().z,layout.SampleSpacing(),minX,minZ,maxX,maxZ,
                        extent.minX-origin.x,extent.minZ-origin.z,extent.maxX-origin.x,extent.maxZ-origin.z,
                        std::max({0.0,minX-(extent.minX-origin.x),(extent.maxX-origin.x)-maxX}),
                        std::max({0.0,minZ-(extent.minZ-origin.z),(extent.maxZ-origin.z)-maxZ}));return {};
                }
                mesh.maxDeviation=*bound;
                std::vector<glm::vec3> normals;normals.reserve(mesh.positions.size());
                for(auto v:mesh.positions)
                {
                    const auto &ref=recipe.layout;
                    const int64_t ix=ref.TileFirstSample(tile.Tile().x)+std::llround(double(v.x)/ref.SampleSpacing());
                    const int64_t iz=ref.TileFirstSample(tile.Tile().z)+std::llround(double(v.z)/ref.SampleSpacing());
                    normals.push_back(final.Normal(ref.SamplePosition(ix),ref.SamplePosition(iz),tile));
                }
                float lo=INFINITY,hi=-INFINITY;for(auto v:mesh.positions){lo=std::min(lo,v.y);hi=std::max(hi,v.y);}
                TerrainTileRecord record;record.tile=tile.Tile();record.layer=layer;record.level=layer-sourceLayer;record.source=false;
                record.samplesPerSide=layout.IntervalsPerTile()+1;record.sampleSpacing=layout.SampleSpacing();
                record.minHeight=lo;record.maxHeight=hi;record.validExtent=*layout.ValidExtent(tile.Tile());
                record.maxDeviation=mesh.maxDeviation;record.sourceChecksum=mesh.sourceChecksum;record.topologyHash=mesh.topologyHash;
                record.vertexCount=static_cast<uint32_t>(mesh.positions.size());record.indexCount=static_cast<uint32_t>(mesh.indices.size());
                record.kind=TerrainPayloadKind::LodHeight;result.payloads.push_back({record,EncodeTerrainLod(mesh)});
                record.kind=TerrainPayloadKind::Normal;result.payloads.push_back({record,EncodeTerrainNormals(mesh,normals)});
                if(layer==sourceLayer)continue;
                const int n=layout.IntervalsPerTile(),ratio=tile.IntervalsPerTile()/n;
                std::vector<float> samples; samples.reserve(size_t(n+1)*(n+1));
                for(int z=0;z<=n;++z)for(int x=0;x<=n;++x)samples.push_back(tile.Samples()[size_t(z*ratio)*(tile.IntervalsPerTile()+1)+x*ratio]);
                auto query=TileHeights::Create(tile.Tile(),n,std::move(samples));
                auto queryBound=query?TerrainRegularDeviation(sourceLayout,tile,layout,*query):std::nullopt;
                if(!query||!queryBound){error="query derivative/error cannot be enclosed";return {};}
                result.queryHeights.push_back(std::move(*query));result.queryLayers.push_back(layer);result.queryErrors.push_back(*queryBound);
            }
        }
        if(regions&&!(result.regions=BakeTerrainRegions(recipe,heights,*regions,error)))return {};
        return result;
    }
}
