/**
 * @file TerrainFinalSurface.h
 * @author Jackey Cen
 * @brief Final source heights and normals that tile LODs and Regions share bit for bit.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "TerrainRecipe.h"

#include <terrain/TileAddressing.h>
#include <terrain/TileHeights.h>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>

namespace mir::terrainbake
{
    struct TerrainFinalSurface
    {
        const TerrainRecipe &recipe;
        std::map<std::pair<int32_t,int32_t>,const TileHeights *> tiles;
        const TileHeights *Tile(TileCoord t) const
        {
            auto it=tiles.find({t.z,t.x});return it==tiles.end()?nullptr:it->second;
        }
        TerrainLayout Layout(const TileHeights &tile) const { return *recipe.SourceLayout(tile.Tile()); }
        double Height(const TileHeights &tile,double x,double z) const
        {
            const auto layout=Layout(tile);const int n=tile.IntervalsPerTile();
            const double ox=layout.SamplePosition(layout.TileFirstSample(tile.Tile().x));
            const double oz=layout.SamplePosition(layout.TileFirstSample(tile.Tile().z));
            // Include stored padding for a clipped outer cell.
            const double ux=std::clamp((x-ox)/layout.SampleSpacing(),0.0,double(n));
            const double uz=std::clamp((z-oz)/layout.SampleSpacing(),0.0,double(n));
            const int ix=std::min(int(std::floor(ux)),n-1),iz=std::min(int(std::floor(uz)),n-1);
            const double fx=ux-ix,fz=uz-iz;
            auto at=[&](int a,int b){return double(tile.Samples()[size_t(b)*(n+1)+a]);};
            const double a=at(ix,iz),c=at(ix+1,iz+1);
            return fz>=fx ? a+fz*(at(ix,iz+1)-a)+fx*(c-at(ix,iz+1)) :
                            a+fx*(at(ix+1,iz)-a)+fz*(c-at(ix+1,iz));
        }
        const TileHeights *Owner(double x,double z,const TileHeights &fallback) const
        {
            const auto a=AddressLocal(recipe.layout,*TerrainLocalPosition::FromMeters({x,0,z}));
            return a.IsInside()&&Tile(a.tile)?Tile(a.tile):&fallback;
        }
        double SharedHeight(double x,double z,const TileHeights &fallback) const
        {
            return Height(*Owner(x,z,fallback),x,z);
        }
        glm::vec3 Normal(double x,double z,const TileHeights &fallback) const
        {
            // Incident source grids determine a common, LOD-independent stencil.
            double step=Layout(fallback).SampleSpacing();
            const auto &ref=recipe.layout;const double size=ref.TileSize();
            for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx)
            {
                const int64_t tx=int64_t(fallback.Tile().x)+dx,tz=int64_t(fallback.Tile().z)+dz;
                if(tx<INT32_MIN||tx>INT32_MAX||tz<INT32_MIN||tz>INT32_MAX)continue;
                const TileCoord t{int32_t(tx),int32_t(tz)};const auto *other=Tile(t);if(!other)continue;
                const double ax=ref.SamplePosition(ref.TileFirstSample(t.x)),az=ref.SamplePosition(ref.TileFirstSample(t.z));
                const double eps=64*std::numeric_limits<double>::epsilon()*std::max({1.0,std::abs(x),std::abs(z),size});
                if(x>=ax-eps&&x<=ax+size+eps&&z>=az-eps&&z<=az+size+eps)
                    step=std::max(step,Layout(*other).SampleSpacing());
            }
            const auto b=ref.Bounds();
            const double xl=std::clamp(x-step,b.minX,b.maxX),xr=std::clamp(x+step,b.minX,b.maxX);
            const double zl=std::clamp(z-step,b.minZ,b.maxZ),zr=std::clamp(z+step,b.minZ,b.maxZ);
            double gx=xr>xl?(SharedHeight(xr,std::clamp(z,b.minZ,b.maxZ),fallback)-SharedHeight(xl,std::clamp(z,b.minZ,b.maxZ),fallback))/(xr-xl):0;
            double gz=zr>zl?(SharedHeight(std::clamp(x,b.minX,b.maxX),zr,fallback)-SharedHeight(std::clamp(x,b.minX,b.maxX),zl,fallback))/(zr-zl):0;
            return glm::vec3(glm::normalize(glm::dvec3(-gx,1,-gz)));
        }
    };
}
