#include "waypointmodule.hpp"
#include "../visual/blockoutline_geometry.hpp"
#include "../../core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <utility>
#include "../ModuleRegistry.hpp"
#include <string>

namespace {
using BeginFn = void(*)(void*, void*, int, int, int);
using ColorFn = void(*)(void*, float, float, float, float);
using VertexFn = void(*)(void*, float, float, float);
using MeshFn = void(*)(void*, void*, void*, char*);
using RenderLevelFn = void(*)(void*, void*, void*);
using HitFn = void*(*)(void*);

struct HashedString { std::uint64_t hash{}; std::string text; const HashedString* last{};
    explicit HashedString(const char* s) : text(s ? s : "") { constexpr std::uint64_t o=0xcbf29ce484222325ULL,p=0x100000001b3ULL; hash=o; for(char c:text) hash=(unsigned char)c^(p*hash); }
};
struct MaterialPtr { void* p[2]{}; explicit operator bool() const { return p[0] != nullptr; } };

WaypointModule* g_module = nullptr;
BeginFn g_begin = nullptr; ColorFn g_color = nullptr; VertexFn g_vertex = nullptr; MeshFn g_mesh = nullptr; HitFn g_hit = nullptr;
RenderLevelFn g_original = nullptr;
MaterialPtr g_selection; std::uintptr_t g_materialGroup = 0;
bedrocktools::sdk::Vec3 g_player{}; int g_dimension = 0; bool g_hasTarget = false; BlockPosition g_target{};

std::uintptr_t resolveAdrp(std::uint32_t* code) {
    for (int i=0;i<2;i++) { auto ins=code[i]; if ((ins&0x1f)!=0) continue;
        if ((ins&0x9f000000)==0x90000000) { auto page=(std::uintptr_t)&code[i]&~0xfffULL; page += (std::int64_t)((std::uint64_t)(((ins>>3)&0x1ffffc)|((ins>>29)&3))<<43)>>31; auto add=code[i+1]; if ((add&0xff000000)==0x91000000) return page+((add>>10)&0xfff); }
    } return 0;
}
void updateTarget(bedrocktools::sdk::Player* player) {
    if (!g_module || !player || !g_hit) { g_hasTarget=false; return; }
    g_player=player->position();
    if (auto* d=player->dimension(); d && d->blockSource()) g_dimension=d->blockSource()->dimensionId();
    auto level=*reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(player)+bedrocktools::sdk::offsets::Actor::mLevel);
    void* hit=level ? g_hit(level) : nullptr; if (!hit) {g_hasTarget=false; return;}
    auto a=reinterpret_cast<std::uintptr_t>(hit); if (*reinterpret_cast<int*>(a+bedrocktools::sdk::offsets::HitResult::mType)!=bedrocktools::sdk::offsets::HitResult::TypeBlock) {g_hasTarget=false; return;}
    auto p=*reinterpret_cast<const bedrocktools::sdk::BlockPos*>(a+bedrocktools::sdk::offsets::HitResult::mBlockPos); g_target={p.x,p.y,p.z}; g_hasTarget=true;
}
MaterialPtr getMaterial(const char* name) {
    if (!g_materialGroup) return {}; HashedString hs(name); auto table=*reinterpret_cast<void***>(g_materialGroup); if (!table || !table[2]) return {};
    using Get=MaterialPtr(*)(void*,const HashedString*); return reinterpret_cast<Get>(table[2])(reinterpret_cast<void*>(g_materialGroup),&hs);
}
void drawBox(void* context, void* tess, void* material, const BlockPosition& p, const BlockWaypoint& wp, float cx,float cy,float cz) {
    // The shared block geometry is expanded by the configured amount. This keeps
    // the outline above the block surface on GLES devices where line width is
    // not portable, while preserving the same cached twelve-edge topology.
    const float expansion = .002f + std::max(0.f, wp.outlineThickness - 1.f) * .006f;
    const auto box=bedrocktools::modules::blockoutline::makeBox((float)p.x,(float)p.y,(float)p.z, expansion);
    const auto visible=bedrocktools::modules::blockoutline::makeEdgeVisibility(box,{cx,cy,cz});
    int count=0; for(bool v:visible) if(v) ++count;
    char pad[0x58]{};
    if (wp.outline && count) { g_begin(tess,nullptr,4,count*2,0); g_color(tess,wp.r,wp.g,wp.b,wp.outlineOpacity); for(std::size_t i=0;i<box.size();++i) if(visible[i]) { const auto&a=box[i]; g_vertex(tess,a.from.x-cx,a.from.y-cy,a.from.z-cz); g_vertex(tess,a.to.x-cx,a.to.y-cy,a.to.z-cz); } g_mesh(context,tess,material,pad); }
    if (wp.fill) { const auto faces=bedrocktools::modules::blockoutline::makeFaces((float)p.x,(float)p.y,(float)p.z,.002f); const auto fv=bedrocktools::modules::blockoutline::makeFaceVisibility(faces,{cx,cy,cz}); int n=0; for(bool v:fv) if(v) ++n; if(n) { g_begin(tess,nullptr,1,n*4,0); g_color(tess,wp.r,wp.g,wp.b,wp.fillOpacity); for(std::size_t i=0;i<faces.size();++i) if(fv[i]) { const auto&q=faces[i]; g_vertex(tess,q.a.x-cx,q.a.y-cy,q.a.z-cz); g_vertex(tess,q.b.x-cx,q.b.y-cy,q.b.z-cz); g_vertex(tess,q.c.x-cx,q.c.y-cy,q.c.z-cz); g_vertex(tess,q.d.x-cx,q.d.y-cy,q.d.z-cz); } g_mesh(context,tess,material,pad); } }
}
void renderHook(void* renderer, void* context, void* params) {
    if(g_original) g_original(renderer,context,params);
    if(!g_module || !g_module->enabled || !context || !g_begin || !g_color || !g_vertex || !g_mesh) return;
    auto list=WaypointManager::get().snapshot(); if(list.empty()) return;
    auto ra=reinterpret_cast<std::uintptr_t>(renderer); auto lr=*reinterpret_cast<void**>(ra+bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer); if(!lr) return;
    auto camera=*reinterpret_cast<const bedrocktools::sdk::Vec3*>(reinterpret_cast<std::uintptr_t>(lr)+bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);
    auto tess=*reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(context)+bedrocktools::sdk::offsets::ScreenContext::mTessellator); if(!tess) return;
    if(!g_selection && g_materialGroup) g_selection=getMaterial("selection_box"); void* mat=g_selection ? &g_selection : reinterpret_cast<char*>(lr)+bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial;
    const float maxD=std::max(1.f,g_module->maxRenderDistance); const float maxSq=maxD*maxD;
    for(const auto& wp:list) { if(!wp.enabled || wp.dimensionId!=g_dimension || wp.blocks.empty()) continue; float nearestSq=INFINITY; for(const auto&p:wp.blocks) {float dx=p.x+.5f-g_player.x,dy=p.y+.5f-g_player.y,dz=p.z+.5f-g_player.z; nearestSq=std::min(nearestSq,dx*dx+dy*dy+dz*dz);} if(nearestSq>maxSq) continue; for(const auto&p:wp.blocks) drawBox(context,tess,mat,p,wp,camera.x,camera.y,camera.z); }
}
std::string hex(float r,float g,float b) { char s[16]; std::snprintf(s,sizeof(s),"#%02X%02X%02X",(int)(std::clamp(r,0.f,1.f)*255),(int)(std::clamp(g,0.f,1.f)*255),(int)(std::clamp(b,0.f,1.f)*255)); return s; }
void rgb(const std::string&s,float&r,float&g,float&b) { if(s.size()!=7||s[0]!='#')return; try {auto v=std::stoul(s.substr(1),nullptr,16);r=((v>>16)&255)/255.f;g=((v>>8)&255)/255.f;b=(v&255)/255.f;}catch(...){} }
}

WaypointModule::WaypointModule() : Module("WaypointModule","Block-based waypoints: one named waypoint can contain unlimited selected blocks.") { g_module=this; }
WaypointModule::~WaypointModule() { if(g_module==this)g_module=nullptr; }
void WaypointModule::onInit() {
    auto resolve=[](bedrocktools::memory::SignatureId id){return bedrocktools::memory::resolve(id);};
    auto r=resolve(bedrocktools::memory::SignatureId::RenderLevel); auto b=resolve(bedrocktools::memory::SignatureId::TessellatorBegin); auto c=resolve(bedrocktools::memory::SignatureId::TessellatorColor); auto v=resolve(bedrocktools::memory::SignatureId::TessellatorVertex); auto m=resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2); if(!m)m=resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately); auto h=resolve(bedrocktools::memory::SignatureId::LevelGetHitResult); auto mg=resolve(bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);
    g_begin=(BeginFn)b;g_color=(ColorFn)c;g_vertex=(VertexFn)v;g_mesh=(MeshFn)m;g_hit=(HitFn)h; if(mg) {g_materialGroup=resolveAdrp(reinterpret_cast<std::uint32_t*>(mg)); if(g_materialGroup)g_materialGroup+=bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;}
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto&e){updateTarget(e.player);});
    if(r) { bedrocktools::hooks::install(reinterpret_cast<void*>(r),reinterpret_cast<void*>(renderHook),reinterpret_cast<void**>(&g_original)); m_hookInstalled=true; }
}
void WaypointModule::onEnable() {}
void WaypointModule::onFrame() {
    handleActions();
    if (!enabled || !showLabel) return;
    const auto list = WaypointManager::get().snapshot();
    const BlockWaypoint* nearest = nullptr;
    float nearestSq = INFINITY;
    for (const auto& wp : list) {
        if (!wp.enabled || wp.dimensionId != g_dimension || wp.blocks.empty()) continue;
        float d = INFINITY;
        for (const auto& p : wp.blocks) {
            const float dx = p.x + .5f - g_player.x, dy = p.y + .5f - g_player.y, dz = p.z + .5f - g_player.z;
            d = std::min(d, dx * dx + dy * dy + dz * dz);
        }
        if (d < nearestSq && d <= maxRenderDistance * maxRenderDistance) { nearestSq = d; nearest = &wp; }
    }
    if (!nearest) { submitDrawCommands(moduleId, {}); return; }
    std::vector<PLModMenu_DrawCommand> commands;
    PLModMenu_DrawCommand background{}; background.type = PL_DRAW_RECT_FILLED; background.x = hudPosX; background.y = hudPosY; background.w = 180.f; background.h = 34.f; background.color = 0x99000000; commands.push_back(background);
    std::string label = "[ " + nearest->name + " ]";
    if (showDistance) label += "  " + std::to_string((int)std::round(std::sqrt(nearestSq))) + "m";
    PLModMenu_DrawCommand text{}; text.type = PL_DRAW_TEXT; text.x = hudPosX + 8.f; text.y = hudPosY + 7.f; text.w = 164.f; text.h = 22.f; text.size = 14.f; text.color = 0xFFFFFFFF; text.text = label.c_str(); commands.push_back(text);
    submitDrawCommands(moduleId, commands);
}
void WaypointModule::handleActions() {
    auto list=WaypointManager::get().snapshot();
    if(startSelectionButton) { m_selection.clear(); startSelectionButton=false; }
    if(addBlockButton) { if(g_hasTarget) {
        auto existing = std::find_if(list.begin(), list.end(), [&](const auto& w) { return w.name == waypointName; });
        if (existing != list.end()) {
            if (std::find(existing->blocks.begin(), existing->blocks.end(), g_target) == existing->blocks.end()) existing->blocks.push_back(g_target);
            WaypointManager::get().replace(list);
        } else {
            if(mode.rfind("Single", 0) == 0) m_selection.clear();
            if(std::find(m_selection.begin(),m_selection.end(),g_target)==m_selection.end()) m_selection.push_back(g_target);
        }
    } addBlockButton=false; }
    if(removeBlockButton) { auto existing=std::find_if(list.begin(),list.end(),[&](const auto&w){return w.name==waypointName;}); if(existing!=list.end()&&!existing->blocks.empty()){existing->blocks.pop_back();WaypointManager::get().replace(list);} else if(!m_selection.empty())m_selection.pop_back(); removeBlockButton=false; }
    if(clearBlocksButton) { auto existing=std::find_if(list.begin(),list.end(),[&](const auto&w){return w.name==waypointName;}); if(existing!=list.end()){existing->blocks.clear();WaypointManager::get().replace(list);} m_selection.clear();clearBlocksButton=false;}
    if(createWaypointButton && !m_selection.empty()) {list.push_back(makeWaypoint(waypointName,m_selection,color,fill,outline,fillOpacity,outlineOpacity,outlineThickness,g_dimension));WaypointManager::get().replace(list);m_selection.clear();createWaypointButton=false;}
    if(deleteButton) { auto existing=std::find_if(list.begin(),list.end(),[&](const auto&w){return w.name==waypointName;}); if(existing!=list.end()) list.erase(existing); WaypointManager::get().replace(list);deleteButton=false;}
    if(saveButton) saveButton=false;
}
BlockWaypoint WaypointModule::makeWaypoint(const std::string&n,const std::vector<BlockPosition>&b,const std::string&col,bool f,bool o,float fa,float oa,float ot,int dim) { BlockWaypoint w;w.name=n.empty()?"Waypoint":n;w.blocks=b;rgb(col,w.r,w.g,w.b);w.fill=f;w.outline=o;w.fillOpacity=std::clamp(fa,0.f,1.f);w.outlineOpacity=std::clamp(oa,0.f,1.f);w.outlineThickness=std::clamp(ot,1.f,10.f);w.dimensionId=dim;return w; }
void WaypointModule::loadConfig(const nlohmann::json&j) { Module::loadConfig(j); mode=j.value("mode",mode);waypointName=j.value("waypointName",waypointName);color=j.value("color",color);fill=j.value("fill",fill);outline=j.value("outline",outline);fillOpacity=j.value("fillOpacity",fillOpacity);outlineOpacity=j.value("outlineOpacity",outlineOpacity);outlineThickness=j.value("outlineThickness",outlineThickness);maxRenderDistance=j.value("maxRenderDistance",maxRenderDistance);showLabel=j.value("showLabel",showLabel);showDistance=j.value("showDistance",showDistance);hudPosX=j.value("hudPosX",hudPosX);hudPosY=j.value("hudPosY",hudPosY); auto it=j.find("waypoints"); if(it!=j.end()&&it->is_array()){std::vector<BlockWaypoint> w;for(const auto&x:*it)w.push_back(WaypointManager::fromJson(x));WaypointManager::get().replace(std::move(w));} handleActions(); }
void WaypointModule::saveConfig(nlohmann::json&j) { Module::saveConfig(j);j["mode"]=mode;j["waypointName"]=waypointName;j["color"]=color;j["fill"]=fill;j["outline"]=outline;j["fillOpacity"]=fillOpacity;j["outlineOpacity"]=outlineOpacity;j["outlineThickness"]=outlineThickness;j["maxRenderDistance"]=maxRenderDistance;j["showLabel"]=showLabel;j["showDistance"]=showDistance;j["hudPosX"]=hudPosX;j["hudPosY"]=hudPosY;j["createWaypointButton"]=false;j["startSelectionButton"]=false;j["addBlockButton"]=false;j["removeBlockButton"]=false;j["clearBlocksButton"]=false;j["saveButton"]=false;j["deleteButton"]=false;nlohmann::json a=nlohmann::json::array();for(const auto&w:WaypointManager::get().snapshot()){nlohmann::json x;WaypointManager::toJson(w,x);a.push_back(std::move(x));}j["waypoints"]=std::move(a);}
