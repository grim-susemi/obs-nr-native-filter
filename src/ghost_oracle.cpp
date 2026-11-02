// SPDX-License-Identifier: GPL-3.0-only
#include "ghost_oracle.h"
#include "ghost_freeze.h"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace nr::ghost {
namespace {
Rgb sub(Rgb a, Rgb b) { return {a.r-b.r,a.g-b.g,a.b-b.b}; }
double dot(Rgb a, Rgb b) { return a.r*b.r+a.g*b.g+a.b*b.b; }
double inf(Rgb a) { return std::max({std::abs(a.r),std::abs(a.g),std::abs(a.b)}); }
bool finite(Rgb a) {
    constexpr uint64_t exponent=0x7ff0000000000000ull;
    return (std::bit_cast<uint64_t>(a.r)&exponent)!=exponent&&
        (std::bit_cast<uint64_t>(a.g)&exponent)!=exponent&&
        (std::bit_cast<uint64_t>(a.b)&exponent)!=exponent;
}
void image_size(const Image& a)
{
    if(a.empty()) throw std::invalid_argument("oracle image extent");
    for (auto c:a) if(!finite(c)) throw std::invalid_argument("nonfinite oracle image");
}
}
Calibration calibrate(double declaredFormatBound, std::span<const Image> clean, std::span<const Image> raw)
{
    if(!std::isfinite(declaredFormatBound)||declaredFormatBound<0||clean.empty()||clean.size()!=raw.size())
        throw std::invalid_argument("raw-only calibration cohort");
    double error=0;
    for(size_t t=0;t<clean.size();++t) {
        image_size(clean[t]); image_size(raw[t]);
        if(clean[t].size()!=raw[t].size()) throw std::invalid_argument("calibration image extent mismatch");
        for(size_t p=0;p<clean[t].size();++p) error=std::max(error,inf(sub(raw[t][p],clean[t][p])));
    }
    return {declaredFormatBound,error,std::max(declaredFormatBound,error)};
}
bool transport(const Image& clean,const Image& raw,double declaredBound)
{
    image_size(clean); image_size(raw);
    if(clean.size()!=raw.size()) return false;
    if(!std::isfinite(declaredBound)||declaredBound<0) return false;
    for(size_t p=0;p<clean.size();++p) if(inf(sub(raw[p],clean[p]))>declaredBound) return false;
    return true;
}
TemporalReport temporal(const Fixture& f,unsigned tick,PixelView raw,PixelView output,const Calibration& calibration)
{
    if(!raw.pixels||!output.pixels||!raw.read||!output.read||raw.extent!=output.extent||
        raw.extent.width<Fixture::width||raw.extent.height<Fixture::height)
        throw std::invalid_argument("temporal validated pixel view");
    if(!std::isfinite(calibration.epsilon)||calibration.epsilon<0||
        calibration.epsilon!=std::max(calibration.formatBound,calibration.rawMaxError))
        throw std::invalid_argument("unfrozen epsilon");
    const auto current=clean_image(f,tick);
    TemporalReport report;
    report.currentTick=tick;
    std::vector<unsigned> distinct;
    for(unsigned prior=0;prior<tick;++prior) {
        const auto content=f.schedule[prior].contentId;
        if(std::find(distinct.begin(),distinct.end(),content)!=distinct.end()) continue;
        distinct.push_back(content);
        for(unsigned region=0;region<2;++region) {
            TemplateScore score{tick,prior,region,0,0,0,0,0,0,false,true};
            double norm1=0, incremental=0, rawTrail=0, total=0;
            const auto& old=f.schedule[prior];
            const unsigned paintY=static_cast<unsigned>(old.bar.y+1);
            const unsigned paintX=static_cast<unsigned>(old.bar.x+1);
            std::array<Rgb,2> paint{};
            paint[(paintX+paintY)&1]=clean_pixel(f,old,paintX,paintY);
            paint[((paintX+1)+paintY)&1]=clean_pixel(f,old,paintX+1,paintY);
            const auto box=region==0?old.bar:old.silhouette;
            const auto firstY=static_cast<unsigned>(std::max(8,box.y));
            const auto lastY=static_cast<unsigned>(std::min(static_cast<int>(Fixture::height),box.y+box.height));
            const auto firstX=static_cast<unsigned>(std::max(0,box.x));
            const auto lastX=static_cast<unsigned>(std::min(static_cast<int>(Fixture::width),box.x+box.width));
            // Same y/x accumulation order and exact mask, excluding only points
            // the frozen full-canvas loop always skipped.
            for(unsigned y=firstY;y<lastY;++y)for(unsigned x=firstX;x<lastX;++x) {
                const bool footprint=region==0 ? inside(old.bar,x,y) :
                    foreground(old,x,y)&&!inside(old.bar,x,y);
                if(!visible(x,y)||!footprint||foreground(f.schedule[tick],x,y)) continue;
                const size_t analytic=y*Fixture::width+x;
                // Every admitted old-mask pixel is foreground on rows20..67;
                // all later header/HUD/highlight overrides are disjoint.
                const auto h=sub(paint[(x+y)&1],current[analytic]);
                const auto measuredRaw=raw.read(raw.pixels,x,y),measuredOutput=output.read(output.pixels,x,y);
                if(!finite(measuredRaw)||!finite(measuredOutput))throw std::invalid_argument("nonfinite temporal mask pixel");
                ++score.pixels;
                score.denominator+=dot(h,h);
                norm1+=std::abs(h.r)+std::abs(h.g)+std::abs(h.b);
                incremental+=std::max(0.0,dot(sub(measuredOutput,measuredRaw),h));
                rawTrail+=std::max(0.0,dot(sub(measuredRaw,current[analytic]),h));
                total+=std::max(0.0,dot(sub(measuredOutput,current[analytic]),h));
            }
            score.zeroContrast=score.denominator==0;
            if(!score.zeroContrast) {
                score.bound=temporal_epsilon_factor*calibration.epsilon*norm1/score.denominator;
                score.incremental=incremental/score.denominator;
                score.raw=rawTrail/score.denominator;
                score.total=total/score.denominator;
                score.pass=score.incremental<=score.bound;
                report.maximum=std::max(report.maximum,score.incremental);
                report.maximumRaw=std::max(report.maximumRaw,score.raw);
                report.maximumTotal=std::max(report.maximumTotal,score.total);
                report.pass=report.pass&&score.pass;
            }
            report.templates.push_back(score); // Never average or omit a brief offending region.
        }
    }
    return report;
}
TemporalReport temporal(const Fixture& f,unsigned tick,const Image& raw,const Image& output,
    const Calibration& calibration,Extent canvas)
{
    image_size(raw);image_size(output);
    if(canvas.width<Fixture::width||canvas.height<Fixture::height||
        raw.size()!=static_cast<size_t>(canvas.width)*canvas.height||output.size()!=raw.size())
        throw std::invalid_argument("temporal working canvas");
    struct Pixels { const Image& image;unsigned stride; };
    const Pixels a{raw,canvas.width},b{output,canvas.width};
    auto sample=[](const void* opaque,unsigned x,unsigned y) {
        const auto& view=*static_cast<const Pixels*>(opaque);
        return view.image[static_cast<size_t>(y)*view.stride+x];
    };
    return temporal(f,tick,PixelView{&a,canvas,sample},PixelView{&b,canvas,sample},calibration);
}
unsigned persistence(std::span<const TemporalReport> reports)
{
    unsigned longest=0,current=0;
    std::optional<unsigned> previous;
    for(const auto& report:reports) {
        if(previous&&report.currentTick!=*previous+1) current=0;
        current=report.pass ? 0 : current+1;
        longest=std::max(longest,current);
        previous=report.currentTick;
    }
    return longest;
}
unsigned template_persistence(std::span<const TemporalReport> reports,unsigned priorTick,unsigned region)
{
    unsigned longest=0,current=0;
    std::optional<unsigned> previous;
    for(const auto& report:reports) {
        if(previous&&report.currentTick!=*previous+1) current=0;
        const auto score=std::find_if(report.templates.begin(),report.templates.end(),[&](const TemplateScore& s) {
            return s.priorTick==priorTick&&s.region==region;
        });
        current=score==report.templates.end()||score->pass?0:current+1;
        longest=std::max(longest,current);
        previous=report.currentTick;
    }
    return longest;
}
std::vector<Edge> detail_edges(const Fixture& f,unsigned tick)
{
    const auto image=clean_image(f,tick);
    std::vector<Edge> result;
    for(unsigned y=8;y<Fixture::height;++y) for(unsigned x=0;x<Fixture::width;++x) {
        const size_t p=y*Fixture::width+x;
        if(x+1<Fixture::width&&inf(sub(image[p],image[p+1]))>0) result.push_back({p,p+1});
        if(y+1<Fixture::height&&inf(sub(image[p],image[p+Fixture::width]))>0) result.push_back({p,p+Fixture::width});
    }
    return result;
}
bool detail(const Image& clean,const Image& output,std::span<const Edge> edges,double epsilon)
{
    image_size(clean); image_size(output);
    if(clean.size()!=output.size()) return false;
    if(!std::isfinite(epsilon)||epsilon<0||edges.empty()) return false;
    for(auto edge:edges) {
        if(edge.a>=clean.size()||edge.b>=clean.size()) return false;
        const auto original=sub(clean[edge.a],clean[edge.b]), actual=sub(output[edge.a],output[edge.b]);
        const std::array<double,3> c{original.r,original.g,original.b}, y{actual.r,actual.g,actual.b};
        for(size_t i=0;i<3;++i)
            if(c[i]!=0&&(std::abs(c[i])-(c[i]>0?y[i]:-y[i]))>detail_epsilon_factor*epsilon) return false;
    }
    return true;
}
EditScore edit(const Image& raw,const Image& output,const Image& delta,std::span<const size_t> roi,double epsilon)
{
    image_size(raw); image_size(output); image_size(delta);
    if(raw.size()!=output.size()||raw.size()!=delta.size()) throw std::invalid_argument("edit image extent mismatch");
    if(roi.empty()||!std::isfinite(epsilon)||epsilon<0) return {0,0,false};
    double actual=0,intended=0;
    for(auto p:roi) {
        if(p>=raw.size()) throw std::invalid_argument("edit ROI extent");
        const auto d=sub(output[p],raw[p]);
        actual+=dot(d,d); intended+=dot(delta[p],delta[p]);
    }
    const double rms=std::sqrt(actual/static_cast<double>(roi.size()));
    const double retained=intended==0?0:actual/intended;
    return {rms,retained,intended>0&&rms>edit_epsilon_factor*epsilon&&retained>=.8};
}
GuideCalibration calibrate_guides(std::span<const GuidePoint> points)
{
    double mismatch=0,absolute=0,relative=0;
    bool visiblePoint=false;
    for(const auto& p:points) if(p.labeledVisible) {
        if(!finite(p.current)||!finite(p.warpedReference)||!std::isfinite(p.x)||!std::isfinite(p.y)||
            !std::isfinite(p.expectedX)||!std::isfinite(p.expectedY))
            throw std::invalid_argument("nonfinite guide calibration");
        visiblePoint=true;
        mismatch=std::max(mismatch,inf(sub(p.current,p.warpedReference)));
        const double error=std::max(std::abs(p.x-p.expectedX),std::abs(p.y-p.expectedY));
        absolute=std::max(absolute,error);
        relative=std::max(relative,error/std::max({1.0,std::abs(p.expectedX),std::abs(p.expectedY)}));
    }
    if(!visiblePoint) throw std::invalid_argument("no labeled visible guide calibration");
    return {mismatch,absolute,relative};
}
bool guide_point(const GuidePoint& p,const GuideCalibration& c)
{
    if(!p.labeledVisible||!finite(p.current)||!finite(p.warpedReference)||!std::isfinite(p.x)||!std::isfinite(p.y)||
        !std::isfinite(p.expectedX)||!std::isfinite(p.expectedY)) return false;
    const double error=std::max(std::abs(p.x-p.expectedX),std::abs(p.y-p.expectedY));
    return inf(sub(p.current,p.warpedReference))<=c.mismatch&&error<=c.absoluteError&&
        error/std::max({1.0,std::abs(p.expectedX),std::abs(p.expectedY)})<=c.relativeError;
}
bool valid_support(std::span<const bool> support)
{
    return !support.empty()&&std::all_of(support.begin(),support.end(),[](bool valid){return valid;});
}
double decoded_vector(int16_t value,double activeScale)
{
    if(!std::isfinite(activeScale)||activeScale<=0) throw std::invalid_argument("guide rectangle scale");
    return static_cast<double>(value)/32*activeScale;
}
} // namespace nr::ghost
