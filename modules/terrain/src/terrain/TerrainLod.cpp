#include <terrain/TerrainLod.h>
#include "LittleEndian.h"
#include "PackInternal.h"
#include "TerrainOverlay.h"
#include <assets/AssetId.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <glm/vec2.hpp>
#include <glm/geometric.hpp>

namespace mir
{
    namespace
    {
        using V = glm::dvec3;
        using P = glm::dvec2;
        using Code = TerrainPackErrorCode;
        constexpr uint32_t kLodMagic = 0x31444f4c;
        constexpr uint32_t kNormalMagic = 0x314d524e;
        double Cross(P a, P b) { return a.x*b.y-a.y*b.x; }
        double Height(const std::array<V,3> &t, P p)
        {
            P a(t[0].x,t[0].z), b(t[1].x,t[1].z), c(t[2].x,t[2].z);
            double d = Cross(b-a,c-a);
            double u = Cross(p-a,c-a)/d, v = Cross(b-a,p-a)/d;
            return t[0].y + u*(t[1].y-t[0].y) + v*(t[2].y-t[0].y);
        }
        struct Polygon
        {
            std::array<P,32> points{};
            size_t count=0;
            bool empty() const {return count==0;}
            P back() const {return points[count-1];}
            const P *begin() const {return points.data();}
            const P *end() const {return points.data()+count;}
            bool Push(P p)
            {
                if(count==points.size())return false;
                points[count++]=p;return true;
            }
        };
        Polygon Clip(Polygon polygon, P a, P b, double &uncertainty)
        {
            if (polygon.empty()) return {};
            Polygon out;
            P prev = polygon.back();
            double dp = Cross(b-a,prev-a);
            auto uncertainSign=[&](P p,double cross){
                double scale=std::abs((b.x-a.x)*(p.y-a.y))+std::abs((b.y-a.y)*(p.x-a.x));
                if(cross!=0 && std::abs(cross)<=64*std::numeric_limits<double>::epsilon()*scale)uncertainty=1;
            };
            uncertainSign(prev,dp);
            for (P point : polygon)
            {
                double dc = Cross(b-a,point-a);uncertainSign(point,dc);
                if ((dc >= 0) != (dp >= 0))
                {
                    const double denominator = dp-dc;
                    if (denominator != 0 && !out.Push(prev+(point-prev)*(dp/denominator)))
                    {uncertainty=1;return {};}
                }
                if (dc >= 0 && !out.Push(point)) {uncertainty=1;return {};}
                prev=point;dp=dc;
            }
            return out;
        }
        void Header(detail::LeWriter &out, uint32_t magic, const TerrainLodSurface &lod, uint32_t count)
        {
            out.U32(magic);out.U32(kTerrainLodContentVersion);out.I32(lod.tile.x);out.I32(lod.tile.z);
            out.F64(lod.sourceSpacing);out.F64(lod.spacing);out.U64(lod.sourceChecksum);out.U64(lod.topologyHash);
            out.U64(0);out.U32(count);out.U32(static_cast<uint32_t>(lod.indices.size()));out.F64(lod.maxDeviation);out.U64(0);
        }
        TerrainPackError CheckHeader(const TerrainIndex &index, const TerrainTileRecord &r,
                                     std::span<const std::byte> bytes, uint32_t magic)
        {
            if (bytes.size() != r.length || bytes.size() < kTerrainDerivedHeaderSize)
                return detail::Error(Code::Corrupt,"derived payload length is invalid");
            if (Fnv1a64(bytes) != r.checksum) return detail::Error(Code::Corrupt,"derived payload checksum differs");
            if (detail::LoadU32(bytes,0)!=magic || detail::LoadU32(bytes,4)!=kTerrainLodContentVersion)
                return detail::Error(Code::Unsupported,"derived payload kind/content version differs");
            const auto *source=index.Find(r.tile);
            if (!source || !source->source || source->kind!=TerrainPayloadKind::Height ||
                detail::LoadI32(bytes,8)!=r.tile.x || detail::LoadI32(bytes,12)!=r.tile.z ||
                detail::LoadF64(bytes,16)!=source->sampleSpacing || detail::LoadF64(bytes,24)!=r.sampleSpacing ||
                detail::LoadU64(bytes,32)!=source->checksum || detail::LoadU64(bytes,40)!=r.topologyHash ||
                detail::LoadU64(bytes,48)!=0 || detail::LoadU32(bytes,56)!=r.vertexCount ||
                detail::LoadU32(bytes,60)!=r.indexCount || !r.maxDeviation ||
                detail::LoadF64(bytes,64)!=*r.maxDeviation || detail::LoadU64(bytes,72)!=0)
                return detail::Error(Code::Corrupt,"derived tile/source/spacing/error provenance differs from index");
            return {};
        }
    }
    uint64_t TerrainFinalHeightChecksum(const TileHeights &tile)
    {
        std::vector<std::byte> bytes;detail::LeWriter out(bytes);
        for(float h:tile.Samples()) out.F32(h);
        return Fnv1a64(bytes);
    }
    uint64_t TerrainTopologyHash(std::span<const glm::vec3> positions, std::span<const uint32_t> indices)
    {
        std::vector<std::byte> bytes;detail::LeWriter out(bytes);
        for(auto v:positions){out.F32(v.x);out.F32(v.z);}
        for(uint32_t i:indices)out.U32(i);
        return Fnv1a64(bytes);
    }
    std::vector<std::byte> EncodeTerrainLod(const TerrainLodSurface &lod)
    {
        std::vector<std::byte> bytes;detail::LeWriter out(bytes);
        Header(out,kLodMagic,lod,static_cast<uint32_t>(lod.positions.size()));
        for(auto v:lod.positions){out.F32(v.x);out.F32(v.y);out.F32(v.z);}
        for(uint32_t i:lod.indices)out.U32(i);
        return bytes;
    }
    std::vector<std::byte> EncodeTerrainNormals(const TerrainLodSurface &lod,std::span<const glm::vec3> normals)
    {
        std::vector<std::byte> bytes;detail::LeWriter out(bytes);
        Header(out,kNormalMagic,lod,static_cast<uint32_t>(normals.size()));
        for(auto v:normals){out.F32(v.x);out.F32(v.y);out.F32(v.z);}
        return bytes;
    }
    TerrainResult<TerrainLodSurface> DecodeTerrainLod(const TerrainIndex &index,const TerrainTileRecord &r,
                                                    std::span<const std::byte> bytes)
    {
        using R=TerrainLodSurface;
        if(r.kind!=TerrainPayloadKind::LodHeight)return detail::Fail<R>(Code::InvalidInput,"not a LodHeight record");
        auto error=CheckHeader(index,r,bytes,kLodMagic);if(error.code!=Code::None)return detail::Failure<R>(error,"");
        uint32_t n=detail::LoadU32(bytes,56),ni=detail::LoadU32(bytes,60);
        if(n<4 || ni<6 || ni%3!=0 || uint64_t(80)+uint64_t(n)*12+uint64_t(ni)*4!=bytes.size())
            return detail::Fail<R>(Code::Corrupt,"LOD vertex/index counts are invalid");
        R lod;lod.release=index.release;lod.tile=r.tile;lod.layer=r.layer;lod.level=r.level;
        lod.sourceSpacing=detail::LoadF64(bytes,16);lod.spacing=r.sampleSpacing;
        lod.sourceChecksum=detail::LoadU64(bytes,32);lod.topologyHash=detail::LoadU64(bytes,40);
        lod.maxDeviation=*r.maxDeviation;
        const auto &layout=index.layers[r.layer];
        lod.origin=*TerrainLocalPosition::FromMeters({layout.SamplePosition(layout.TileFirstSample(r.tile.x)),0,
                    layout.SamplePosition(layout.TileFirstSample(r.tile.z))});
        float lo=std::numeric_limits<float>::infinity(),hi=-lo;
        const float size=static_cast<float>(layout.TileSize());
        for(uint32_t i=0;i<n;++i)
        {
            size_t at=80+size_t(i)*12;
            glm::vec3 p(detail::LoadF32(bytes,at),detail::LoadF32(bytes,at+4),detail::LoadF32(bytes,at+8));
            if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)||p.x<0||p.z<0||p.x>size||p.z>size)
                return detail::Fail<R>(Code::Corrupt,"LOD vertex is nonfinite or outside tile");
            lod.positions.push_back(p);lo=std::min(lo,p.y);hi=std::max(hi,p.y);
        }
        for(uint32_t i=0;i<ni;++i)
        {
            uint32_t id=detail::LoadU32(bytes,80+size_t(n)*12+size_t(i)*4);
            if(id>=n)return detail::Fail<R>(Code::Corrupt,"LOD index exceeds vertex count");
            lod.indices.push_back(id);
        }
        for(size_t i=0;i<ni;i+=3)
        {
            const auto a=lod.positions[lod.indices[i]],b=lod.positions[lod.indices[i+1]],c=lod.positions[lod.indices[i+2]];
            if(Cross(P(b.x-a.x,b.z-a.z),P(c.x-a.x,c.z-a.z))>=0)
                return detail::Fail<R>(Code::Corrupt,"LOD triangle is degenerate or wrong winding");
        }
        if(lo!=r.minHeight||hi!=r.maxHeight||TerrainTopologyHash(lod.positions,lod.indices)!=lod.topologyHash)
            return detail::Fail<R>(Code::Corrupt,"LOD range/topology hash differs");
        return {std::move(lod),{}};
    }
    TerrainResult<TerrainLodNormals> DecodeTerrainNormals(const TerrainIndex &index,const TerrainTileRecord &r,
                                                        std::span<const std::byte> bytes)
    {
        using R=TerrainLodNormals;
        if(r.kind!=TerrainPayloadKind::Normal)return detail::Fail<R>(Code::InvalidInput,"not a Normal record");
        auto error=CheckHeader(index,r,bytes,kNormalMagic);if(error.code!=Code::None)return detail::Failure<R>(error,"");
        uint32_t n=detail::LoadU32(bytes,56),ni=detail::LoadU32(bytes,60);
        const auto *mesh=index.Find(r.tile,r.layer,TerrainPayloadKind::LodHeight,r.level);
        if(!mesh||n<4||ni<6||ni%3!=0||uint64_t(80)+uint64_t(n)*12!=bytes.size()||
           r.minHeight!=mesh->minHeight||r.maxHeight!=mesh->maxHeight||r.maxDeviation!=mesh->maxDeviation)
            return detail::Fail<R>(Code::Corrupt,"normal payload has no matching LOD metadata");
        R normals;normals.release=index.release;normals.tile=r.tile;normals.layer=r.layer;normals.level=r.level;
        normals.sourceSpacing=detail::LoadF64(bytes,16);normals.spacing=r.sampleSpacing;
        normals.sourceChecksum=detail::LoadU64(bytes,32);normals.topologyHash=detail::LoadU64(bytes,40);
        for(uint32_t i=0;i<n;++i)
        {
            size_t at=80+size_t(i)*12;glm::vec3 v(detail::LoadF32(bytes,at),detail::LoadF32(bytes,at+4),detail::LoadF32(bytes,at+8));
            double norm=glm::dot(glm::dvec3(v),glm::dvec3(v));
            if(!std::isfinite(norm)||std::abs(norm-1)>2e-6||v.y<=0)
                return detail::Fail<R>(Code::Corrupt,"normal is nonfinite, not unit, or downward");
            normals.normals.push_back(v);
        }
        return {std::move(normals),{}};
    }
    std::optional<double> EvaluateTerrainLod(const TerrainLodSurface &lod,double x,double z)
    {
        if(!std::isfinite(x)||!std::isfinite(z))return {};
        const P p(x,z);
        for(size_t i=0;i<lod.indices.size();i+=3)
        {
            std::array<V,3> t{V(lod.positions[lod.indices[i]]),V(lod.positions[lod.indices[i+1]]),V(lod.positions[lod.indices[i+2]])};
            bool inside=true;
            for(size_t j=0;j<3;++j)
            {
                const auto a=t[j],b=t[(j+1)%3];
                double v=Cross(P(b.x-a.x,b.z-a.z),p-P(a.x,a.z));
                const double scale=std::abs((b.x-a.x)*(p.y-a.z))+std::abs((b.z-a.z)*(p.x-a.x));
                if(v>64*std::numeric_limits<double>::epsilon()*scale)inside=false;
            }
            if(inside)return Height(t,p);
        }
        return {};
    }
    std::optional<double> detail::OverlayDeviation(const TerrainLayout &layout,const TileHeights &source,
        TileCoord tile,std::span<const glm::dvec3> positions,std::span<const uint32_t> indices,bool requireCover)
    {
        if(source.Tile()!=tile||source.IntervalsPerTile()!=layout.IntervalsPerTile()||indices.size()%3)return {};
        const int n=source.IntervalsPerTile();const double s=layout.SampleSpacing();
        const double ox=layout.SamplePosition(layout.TileFirstSample(tile.x));
        const double oz=layout.SamplePosition(layout.TileFirstSample(tile.z));
        auto coordX=[&](int i){return layout.SamplePosition(layout.TileFirstSample(tile.x)+i)-ox;};
        auto coordZ=[&](int i){return layout.SamplePosition(layout.TileFirstSample(tile.z)+i)-oz;};
        const auto extent=layout.ValidExtent(tile);if(!extent)return {};
        const double minX=extent->minX-ox,maxX=extent->maxX-ox,minZ=extent->minZ-oz,maxZ=extent->maxZ-oz;
        double meshMinX=INFINITY,meshMaxX=-INFINITY,meshMinZ=INFINITY,meshMaxZ=-INFINITY;
        for(const auto &v:positions)
        {
            if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z))return {};
            meshMinX=std::min(meshMinX,v.x);meshMaxX=std::max(meshMaxX,v.x);
            meshMinZ=std::min(meshMinZ,v.z);meshMaxZ=std::max(meshMaxZ,v.z);
        }
        if(requireCover&&(meshMinX>minX||meshMaxX<maxX||meshMinZ>minZ||meshMaxZ<maxZ))return {};
        double bound=0,maxHeight=1,maxCondition=1;
        for(float h:source.Samples())
        {
            if(!std::isfinite(h))return {};
            maxHeight=std::max(maxHeight,std::abs(double(h)));
        }
        for(size_t ti=0;ti<indices.size();ti+=3)
        {
            std::array<V,3> a;
            for(int j=0;j<3;++j){if(indices[ti+j]>=positions.size())return {};a[j]=positions[indices[ti+j]];maxHeight=std::max(maxHeight,std::abs(a[j].y));}
            const P u(a[1].x-a[0].x,a[1].z-a[0].z),v(a[2].x-a[0].x,a[2].z-a[0].z);
            const double determinant=std::abs(Cross(u,v));
            if(determinant==0)return {};
            const double condition=(std::abs(u.x*v.y)+std::abs(u.y*v.x))/determinant;
            if(!std::isfinite(condition))return {};
            maxCondition=std::max(maxCondition,condition);
            double ax0=std::min({a[0].x,a[1].x,a[2].x}),ax1=std::max({a[0].x,a[1].x,a[2].x});
            double az0=std::min({a[0].z,a[1].z,a[2].z}),az1=std::max({a[0].z,a[1].z,a[2].z});
            if(ax1<minX||ax0>maxX||az1<minZ||az0>maxZ)continue;
            // An unchanged canonical source triangle needs no overlay.
            const int canonicalX=std::clamp(int(std::floor(ax0/s)),0,n-1);
            const int canonicalZ=std::clamp(int(std::floor(az0/s)),0,n-1);
            bool canonical=ax0==coordX(canonicalX)&&ax1==coordX(canonicalX+1)&&
                           az0==coordZ(canonicalZ)&&az1==coordZ(canonicalZ+1);
            bool hasA=false,hasC=false;
            for(const auto &v:a)
            {
                int x=v.x==ax0?canonicalX:canonicalX+1,z=v.z==az0?canonicalZ:canonicalZ+1;
                canonical=canonical&&(v.x==ax0||v.x==ax1)&&(v.z==az0||v.z==az1)&&
                    v.y==double(source.Samples()[size_t(z)*(n+1)+x]);
                hasA=hasA||(v.x==ax0&&v.z==az0);hasC=hasC||(v.x==ax1&&v.z==az1);
            }
            if(canonical&&hasA&&hasC)continue;
            // One-cell enlargement encloses subtraction/float rounding at cell boundaries.
            int ix0=std::clamp(int(std::floor(ax0/s))-1,0,n-1),ix1=std::clamp(int(std::floor(ax1/s))+1,0,n-1);
            int iz0=std::clamp(int(std::floor(az0/s))-1,0,n-1),iz1=std::clamp(int(std::floor(az1/s))+1,0,n-1);
            for(int iz=iz0;iz<=iz1;++iz)for(int ix=ix0;ix<=ix1;++ix)
            {
                double x0=coordX(ix),x1=coordX(ix+1),z0=coordZ(iz),z1=coordZ(iz+1);
                if(x1<ax0||x0>ax1||z1<az0||z0>az1)continue;
                auto value=[&](int x,int z){return double(source.Samples()[size_t(z)*(n+1)+x]);};
                std::array<V,4> corners{V(x0,value(ix,iz),z0),V(x1,value(ix+1,iz),z0),V(x1,value(ix+1,iz+1),z1),V(x0,value(ix,iz+1),z1)};
                for(int k=0;k<2;++k)
                {
                    std::array<V,3> b=k==0?std::array<V,3>{corners[0],corners[2],corners[3]}:std::array<V,3>{corners[0],corners[1],corners[2]};
                    Polygon polygon;
                    for(P p:{P(a[0].x,a[0].z),P(a[2].x,a[2].z),P(a[1].x,a[1].z)})polygon.Push(p);
                    double uncertain=0;
                    for(int e=0;e<3;++e)polygon=Clip(std::move(polygon),P(b[e].x,b[e].z),P(b[(e+1)%3].x,b[(e+1)%3].z),uncertain);
                    polygon=Clip(std::move(polygon),P(minX,minZ),P(maxX,minZ),uncertain);
                    polygon=Clip(std::move(polygon),P(maxX,minZ),P(maxX,maxZ),uncertain);
                    polygon=Clip(std::move(polygon),P(maxX,maxZ),P(minX,maxZ),uncertain);
                    polygon=Clip(std::move(polygon),P(minX,maxZ),P(minX,minZ),uncertain);
                    if(condition>1/(64*std::numeric_limits<double>::epsilon()))uncertain=1;
                    if(uncertain)
                    {
                        double loA=std::min({a[0].y,a[1].y,a[2].y}),hiA=std::max({a[0].y,a[1].y,a[2].y});
                        double loB=std::min({b[0].y,b[1].y,b[2].y}),hiB=std::max({b[0].y,b[1].y,b[2].y});
                        const double bx0=std::min({b[0].x,b[1].x,b[2].x}),bx1=std::max({b[0].x,b[1].x,b[2].x});
                        const double bz0=std::min({b[0].z,b[1].z,b[2].z}),bz1=std::max({b[0].z,b[1].z,b[2].z});
                        const double left=std::nextafter(std::max({ax0,bx0,minX}),-INFINITY);
                        const double right=std::nextafter(std::min({ax1,bx1,maxX}),INFINITY);
                        const double top=std::nextafter(std::max({az0,bz0,minZ}),-INFINITY);
                        const double bottom=std::nextafter(std::min({az1,bz1,maxZ}),INFINITY);
                        if(left>right||top>bottom)continue;
                        double pairBound=0;bool finite=condition<=1/(64*std::numeric_limits<double>::epsilon());
                        for(P p:{P(left,top),P(right,top),P(right,bottom),P(left,bottom)})
                        {
                            const double difference=std::abs(Height(a,p)-Height(b,p));
                            finite=finite&&std::isfinite(difference);if(std::isfinite(difference))pairBound=std::max(pairBound,difference);
                        }
                        bound=std::max(bound,finite?pairBound:std::max(std::abs(hiA-loB),std::abs(hiB-loA)));
                    }
                    else if(!polygon.empty()) for(P p:polygon)
                    {
                        const double difference=std::abs(Height(a,p)-Height(b,p));
                        if(!std::isfinite(difference))return {};
                        bound=std::max(bound,difference);
                    }
                }
            }
        }
        double margin=4096*std::numeric_limits<double>::epsilon()*maxHeight*
            (1+layout.TileSize()/s+std::max(std::abs(ox),std::abs(oz))/s)*maxCondition;
        if(!std::isfinite(bound)||!std::isfinite(margin)||!std::isfinite(bound+margin))return {};
        return std::nextafter(bound+margin,std::numeric_limits<double>::infinity());
    }
    std::optional<double> TerrainLodDeviation(const TerrainLayout &layout,const TileHeights &source,
                                             const TerrainLodSurface &lod)
    {
        std::vector<glm::dvec3> positions;for(auto p:lod.positions)positions.emplace_back(p);
        return detail::OverlayDeviation(layout,source,lod.tile,positions,lod.indices,true);
    }
    std::optional<double> TerrainRegularDeviation(const TerrainLayout &sourceLayout,const TileHeights &source,
        const TerrainLayout &queryLayout,const TileHeights &query)
    {
        if(source.Tile()!=query.Tile()||query.IntervalsPerTile()!=queryLayout.IntervalsPerTile())return {};
        const auto tile=source.Tile();const int n=query.IntervalsPerTile();
        const auto ix=queryLayout.TileFirstSample(tile.x),iz=queryLayout.TileFirstSample(tile.z);
        const double ox=sourceLayout.SamplePosition(sourceLayout.TileFirstSample(tile.x));
        const double oz=sourceLayout.SamplePosition(sourceLayout.TileFirstSample(tile.z));
        std::vector<glm::dvec3> positions;std::vector<uint32_t> indices;
        for(int z=0;z<=n;++z)for(int x=0;x<=n;++x)
            positions.emplace_back(queryLayout.SamplePosition(ix+x)-ox,query.Samples()[size_t(z)*(n+1)+x],queryLayout.SamplePosition(iz+z)-oz);
        for(int z=0;z<n;++z)for(int x=0;x<n;++x)
        {
            uint32_t a=z*(n+1)+x,b=a+1,d=a+n+1,c=d+1;
            for(auto id:{a,d,c,a,c,b})indices.push_back(id);
        }
        return detail::OverlayDeviation(sourceLayout,source,tile,positions,indices,true);
    }
}
