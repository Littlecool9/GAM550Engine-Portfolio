#include "RasterDepthEnvelope.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
    using V = glm::dvec4;
    double Cross(glm::dvec2 a, glm::dvec2 b) { return a.x*b.y-a.y*b.x; }
    double Plane(V p, int plane)
    {
        switch (plane)
        {
        case 0:return p.w-p.z;
        case 1:return p.z;
        case 2:return p.w+p.x;
        case 3:return p.w-p.x;
        case 4:return p.w+p.y;
        default:return p.w-p.y;
        }
    }
    void RasterizeOracle(RasterDepthEnvelope &result, std::array<glm::dvec3,3> nodes,
        uint32_t width,uint32_t height,uint32_t triangle,bool quantized)
    {
        if(quantized)for(auto &node:nodes)
        {
            node.x=std::nearbyint(node.x/result.quantum)*result.quantum;
            node.y=std::nearbyint(node.y/result.quantum)*result.quantum;
        }
        const auto a=nodes[0],b=nodes[1],c=nodes[2];
        const double area=Cross(glm::dvec2(b-a),glm::dvec2(c-a));
        if(!(area>0)||!std::isfinite(area))return;
        const int left=std::max(0,int(std::floor(std::min({a.x,b.x,c.x}))));
        const int right=std::min(int(width)-1,int(std::ceil(std::max({a.x,b.x,c.x}))));
        const int top=std::max(0,int(std::floor(std::min({a.y,b.y,c.y}))));
        const int bottom=std::min(int(height)-1,int(std::ceil(std::max({a.y,b.y,c.y}))));
        for(int y=top;y<=bottom;++y)for(int x=left;x<=right;++x)
        {
            const glm::dvec2 p(x+.5,y+.5);
            const double u=Cross(p-glm::dvec2(a),glm::dvec2(c-a))/area;
            const double v=Cross(glm::dvec2(b-a),p-glm::dvec2(a))/area;
            if(u<0||v<0||u+v>1)continue;
            const double depth=(1-u-v)*a.z+u*b.z+v*c.z;
            auto &cell=result.cells[size_t(y)*width+x];
            double &nearest=quantized?cell.quantizedDepth:cell.idealDepth;
            auto &id=quantized?cell.quantizedTriangle:cell.idealTriangle;
            if(depth>nearest){nearest=depth;id=triangle;}
        }
    }
    std::vector<V> Clip(std::vector<V> polygon, int plane)
    {
        if (polygon.empty())return {};
        std::vector<V> out;
        V previous=polygon.back();double previousSide=Plane(previous,plane);
        for (V current:polygon)
        {
            const double side=Plane(current,plane);
            if ((side>=0)!=(previousSide>=0))
                out.push_back(previous+(current-previous)*(previousSide/(previousSide-side)));
            if (side>=0)out.push_back(current);
            previous=current;previousSide=side;
        }
        return out;
    }
}
RasterDepthEnvelope BuildRasterDepthEnvelope(std::span<const mir::TerrainTileMesh> meshes,
    std::span<const mir::DrawItem> items, const glm::mat4 &viewProj,
    uint32_t width,uint32_t height,uint32_t subPixelBits,const mir::MaterialDesc &material)
{
    if (meshes.size()!=items.size() || subPixelBits>32)
        throw std::invalid_argument("invalid projection envelope inputs");
    RasterDepthEnvelope result;
    result.quantum=std::ldexp(1.0,-int(subPixelBits));
    result.cells.resize(size_t(width)*height);
    const double numeric=64.0*std::numeric_limits<float>::epsilon();
    for (size_t tile=0;tile<meshes.size();++tile)
    {
        const auto &mesh=meshes[tile];
        std::vector<V> projected;
        for (const auto &vertex:mesh.vertices)
            projected.push_back(V(viewProj*(items[tile].model*glm::vec4(vertex.position,1))));
        for(size_t at=0;at<mesh.indices.size();at+=3)
        {
            std::vector<V> polygon{projected[mesh.indices[at]],projected[mesh.indices[at+1]],projected[mesh.indices[at+2]]};
            for(int plane=0;plane<6;++plane)polygon=Clip(std::move(polygon),plane);
            for(size_t fan=1;fan+1<polygon.size();++fan)
            {
                ProjectedTerrainTriangle triangle;
                triangle.tile=mesh.tile;triangle.primitive=uint32_t(at/3);
                const std::array<V,3> clip{polygon[0],polygon[fan],polygon[fan+1]};
                bool finite=true;
                for(size_t i=0;i<3;++i)
                {
                    if (!(clip[i].w>0) || !std::isfinite(clip[i].x) || !std::isfinite(clip[i].y) ||
                        !std::isfinite(clip[i].z) || !std::isfinite(clip[i].w)) {finite=false;break;}
                    const auto ndc=glm::dvec3(clip[i])/clip[i].w;
                    triangle.screen[i]={(ndc.x+1)*width/2,(ndc.y+1)*height/2,ndc.z};
                    const glm::vec3 rasterNdc=glm::vec3(clip[i])/float(clip[i].w);
                    triangle.rasterViewport[i]={double((rasterNdc.x+1.0f)*float(width)*.5f),
                        double((rasterNdc.y+1.0f)*float(height)*.5f),double(rasterNdc.z)};
                    finite&=std::isfinite(triangle.screen[i].x)&&std::isfinite(triangle.screen[i].y)&&std::isfinite(ndc.z);
                }
                if(!finite)continue;
                auto &a=triangle.screen[0];auto &b=triangle.screen[1];auto &c=triangle.screen[2];
                double area=Cross(glm::dvec2(b-a),glm::dvec2(c-a));
                if(area==0||!std::isfinite(area))continue;
                const bool front=material.frontFace==VK_FRONT_FACE_COUNTER_CLOCKWISE?area<0:area>0;
                if(material.cullMode==VK_CULL_MODE_FRONT_AND_BACK ||
                    (front&&(material.cullMode&VK_CULL_MODE_FRONT_BIT)) ||
                    (!front&&(material.cullMode&VK_CULL_MODE_BACK_BIT)))continue;
                if(area<0){std::swap(b,c);std::swap(triangle.rasterViewport[1],triangle.rasterViewport[2]);area=-area;}
                triangle.depthGradient={((b.z-a.z)*(c.y-a.y)-(c.z-a.z)*(b.y-a.y))/area,
                    ((b.x-a.x)*(c.z-a.z)-(c.x-a.x)*(b.z-a.z))/area};
                const auto gradient=triangle.depthGradient;
                const double slope=std::abs(gradient.x)+std::abs(gradient.y);
                if(!std::isfinite(slope))continue;
                const double margin=result.quantum*slope+numeric*std::max({std::abs(a.z),std::abs(b.z),std::abs(c.z)});
                const int left=std::max(0,int(std::floor(std::min({a.x,b.x,c.x})-result.quantum)));
                const int right=std::min(int(width)-1,int(std::ceil(std::max({a.x,b.x,c.x})+result.quantum)));
                const int top=std::max(0,int(std::floor(std::min({a.y,b.y,c.y})-result.quantum)));
                const int bottom=std::min(int(height)-1,int(std::ceil(std::max({a.y,b.y,c.y})+result.quantum)));
                const size_t id=result.triangles.size();result.triangles.push_back(triangle);
                RasterizeOracle(result,triangle.screen,width,height,uint32_t(id),false);
                RasterizeOracle(result,triangle.rasterViewport,width,height,uint32_t(id),true);
                for(int y=top;y<=bottom;++y)for(int x=left;x<=right;++x)
                {
                    const glm::dvec2 p(x+.5,y+.5);
                    bool possible=true,guaranteed=true;
                    for(size_t i=0;i<3;++i)
                    {
                        const auto edge=glm::dvec2(triangle.screen[(i+1)%3]-triangle.screen[i]);
                        const double edgeMargin=result.quantum*(std::abs(edge.x)+std::abs(edge.y));
                        const double side=Cross(edge,p-glm::dvec2(triangle.screen[i]));
                        possible&=side>=-edgeMargin;guaranteed&=side>edgeMargin;
                    }
                    if(!possible)continue;
                    const double depth=a.z+glm::dot(gradient,p-glm::dvec2(a));
                    auto &cell=result.cells[size_t(y)*width+x];++cell.candidates;
                    cell.minDepth=std::min(cell.minDepth,depth-margin);
                    cell.maxDepth=std::max(cell.maxDepth,depth+margin);
                    if(guaranteed)cell.guaranteedMinDepth=std::max(cell.guaranteedMinDepth,depth-margin);
                    if(slope>cell.maxDepthGradient){cell.maxDepthGradient=slope;cell.gradientTriangle=id;}
                }
            }
        }
    }
    return result;
}
