// Included after Require() in the hidden-context GPU regression executable.
void TestNativeEffectShaders(const std::string& data) {
    using namespace tr;
    std::set<std::string> vertices, fragments;
    for (size_t at=0; (at=data.find("#version",at))!=std::string::npos; ++at) {
        const auto end=data.find('\0',at);
        if (end==std::string::npos) continue;
        const auto src=data.substr(at,end-at);
        if (src.find("void main()") == std::string::npos) continue;
        if (src.find("out float vLayer;")!=std::string::npos) vertices.insert(src);
        if (src.find("in float vLayer;")!=std::string::npos) fragments.insert(src);
    }
    int testedVertices=0, testedFragments=0;
    for (const auto& v : vertices) {
        for (const auto& f : fragments) {
            if (!HandProgramCompiles(v,f)) continue;
            std::string pv=v,pf=f;
            if (!effects::Patch(pv,pf)) continue;
            Require(HandProgramCompiles(pv,pf),"enhancement links with real animated/static tile vertex source");
            ++testedVertices; break;
        }
    }
    for (const auto& f : fragments) {
        for (const auto& v : vertices) {
            if (!HandProgramCompiles(v,f)) continue;
            std::string pv=v,pf=f;
            if (!effects::Patch(pv,pf)) continue;
            Require(HandProgramCompiles(pv,pf),"enhancement links with real fog/lighting fragment source");
            ++testedFragments; break;
        }
    }
    Require(testedVertices>20 && testedFragments>20,"real effect shader family was exercised");
    std::printf("effects: %d real vertex sources, %d real fragment sources compiled\n",testedVertices,testedFragments);
}

void TestEnhancedEffects() {
    using namespace tr;
    using namespace tr::effects;
    Require(Classify("TEX\\5134.DDS",0)==Fire && Classify("2/tex/5134.dds",1)==Fire &&
            Classify("3/TEX/5031.DDS",2)==Lamps,"all games recognize effect atlas paths");
    Require(Classify("TEX/8999.DDS",0)==Particles && Classify("TEX/8999.DDS",1)==None,
            "TR1 particles do not relabel TR2 textures");
    Require(Classify("TEX/3400.DDS",2)==Bubbles && Classify("TEX/3544.DDS",2)==Bubbles &&
            Classify("TEX/3404.DDS",2)==None && Classify("TEX/3530.DDS",0)==None,
            "only supplied TR3 bubble ranges qualify");
    Require(Classify("PIX/5134.DDS",0)==None && Classify("TEX/15134.DDS",0)==None &&
            Classify("TEX/5134.dds.bak",0)==None && Classify(nullptr,0)==None,
            "UI, partial filenames and null paths never qualify");
    Table table{};
    for (int i=0;i<kLayers;++i) Set(table,i,static_cast<Kind>(i%5));
    for (int i=0;i<kLayers;++i) Require(Get(table,i)==i%5,"packed kind lookup preserves neighboring layers");
    const auto saved=table;
    Set(table,-1,Fire); Set(table,kLayers,Fire);
    Require(saved==table && Get(table,-1)==None && Get(table,kLayers)==None,"invalid layers fail closed");
    effectruntime::NoteUpload(3,7,Fire);
    effectruntime::NoteUpload(3,8,Bubbles);
    effectruntime::NoteUpload(3,7,None);
    Require(Get(effectruntime::slots[3].kinds,7)==None && Get(effectruntime::slots[3].kinds,8)==Bubbles,
            "an unrelated upload clears only the replaced layer");
    effectruntime::ResetSlot(3);
    Require(Get(effectruntime::slots[3].kinds,8)==None,"texture recreation clears old level classifications");
    {
        effectruntime::LoadScope outer("TEX/5134.DDS",3,7);
        { effectruntime::LoadScope inner("PIX/a.dds",8,0); }
        Require(effectruntime::pending.slot==3 && effectruntime::pending.kind==Fire,"nested loads restore classification scope");
    }
    Require(effectruntime::pending.slot==-1,"load classification cannot leak to later uploads");

    std::string v=R"GLSL(#version 150
out float vLayer;
out vec2 vTexCoord;
uniform vec4 fixture;
void main() {
    vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    vLayer = fixture.z;
    vTexCoord=fixture.xy;
    gl_Position=vec4(p[gl_VertexID],0,1);
}
)GLSL";
    std::string f=R"GLSL(#version 150
uniform sampler2DArray sTex0;
in float vLayer;
in vec2 vTexCoord;
out vec4 fragColor;
void main() { fragColor=texture(sTex0, vec3(vTexCoord.xy, vLayer)); }
)GLSL";
    Require(Patch(v,f) && HandProgramCompiles(v,f),"effect pixel fixture patches and links");
    const auto pv=v,pf=f;
    Require(!Patch(v,f) && pv==v && pf==f,"second patch is rejected without mutating sources");
    GLuint vs=gl::CreateShader(GL_VERTEX_SHADER), fs=gl::CreateShader(GL_FRAGMENT_SHADER);
    const char* vt=v.c_str(); const char* ft=f.c_str();
    gl::ShaderSource(vs,1,&vt,nullptr); gl::CompileShader(vs);
    gl::ShaderSource(fs,1,&ft,nullptr); gl::CompileShader(fs);
    GLuint program=gl::CreateProgram(); gl::AttachShader(program,vs); gl::AttachShader(program,fs); gl::LinkProgram(program);
    gl::UseProgram(program);
    GLuint vao=0, texture=0;
    gl::GenVertexArrays(1,&vao); gl::BindVertexArray(vao);
    gl::ActiveTexture(GL_TEXTURE0); glGenTextures(1,&texture); glBindTexture(GL_TEXTURE_2D_ARRAY,texture);
    glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    using Image3D = void(APIENTRY*)(GLenum,GLint,GLint,GLsizei,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*);
    const auto image3D=reinterpret_cast<Image3D>(wglGetProcAddress("glTexImage3D"));
    Require(image3D && gl::Uniform4iv,"effect GPU fixture API");
    std::array<unsigned char,384*4> texels{};
    for(int i=0;i<384;++i) { texels[i*4]=80;texels[i*4+1]=50;texels[i*4+2]=20;texels[i*4+3]=255; }
    // The last layer verifies packing at the boundary; another layer is empty.
    for(int i=0;i<4;++i) texels[382*4+i]=0;
    texels[381*4+3]=128;
    image3D(GL_TEXTURE_2D_ARRAY,0,GL_RGBA8,1,1,384,0,GL_RGBA,GL_UNSIGNED_BYTE,texels.data());
    gl::Uniform1i(gl::GetUniformLocation(program,"sTex0"),0);
    glViewport(0,0,8,8); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_BLEND);
    const GLint fixture=gl::GetUniformLocation(program,"fixture");
    testShaders[0].id=program; testState.shader=0; testState.tex[0]=3;
    effectruntime::ready=true; testWorld=true; testConfig.enhancedEffects=true;
    EnhancedEffectsInvalidateProgram(program);
    float background=0;
    const auto draw=[&](float x,float y,int layer) {
        float coords[4]={x/512.0f,y/512.0f,static_cast<float>(layer),0};
        gl::Uniform4fv(fixture,1,coords); EnhancedEffectsAfterValidate();
        glClearColor(background,background,background,0); glClear(GL_COLOR_BUFFER_BIT); glDrawArrays(GL_TRIANGLES,0,3);
        std::array<unsigned char,4> pixel{}; glReadPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data()); return pixel;
    };
    const auto native=draw(300,190,7);
    Require(native[0]==80 && native[1]==50 && native[2]==20,"unclassified texture is pixel-identical");
    effectruntime::NoteUpload(3,7,Fire);
    auto fire=draw(300,190,7);
    Require(fire[0]>native[0] && fire[2]<native[2],"fire becomes brighter with warmer edges");
    effectruntime::NoteUpload(3,7,Lamps);
    Require(draw(50,50,7)==native && draw(400,75,7)[0]>native[0],"lamp masks preserve ordinary equipment pixels");
    effectruntime::NoteUpload(3,7,Particles);
    Require(draw(32,270,7)==native && draw(220,450,7)==native && draw(110,110,7)[0]>native[0],
            "particle enhancement preserves footprints and solid strips");
    effectruntime::NoteUpload(3,383,Bubbles);
    Require(draw(100,100,383)[0]>native[0],"last packed layer enhances faint bubble rims");
    effectruntime::NoteUpload(3,382,Fire);
    auto empty=draw(100,100,382);
    Require(empty[0]==0 && empty[1]==0 && empty[2]==0,"transparent black stays empty");
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    effectruntime::NoteUpload(3,381,Fire);
    auto translucent=draw(300,190,381);
    Require(std::abs(int(translucent[0])*2-int(fire[0]))<=2,"enhancement preserves partial alpha coverage");
    background=0.25f;
    empty=draw(100,100,382);
    Require(empty[0]>=63 && empty[0]<=65 && empty[1]==empty[0] && empty[2]==empty[0],
            "transparent sprites cannot paint black quads over the scene");
    glDisable(GL_BLEND); background=0;
    testConfig.enhancedEffects=false;
    Require(draw(110,110,7)==native,"disabling enhancement restores stock pixels");
    testConfig.enhancedEffects=true; testWorld=false;
    Require(draw(110,110,7)==native,"UI draws clear enhancement uniforms");
    testWorld=true; testState.tex[0]=4;
    Require(draw(110,110,7)==native,"switching texture slots cannot leak enhancement");
    testState.tex[0]=3;
    effectruntime::ResetSlot(3);
    Require(draw(110,110,7)==native,"level reload invalidates cached per-program table");
    Require(glGetError()==GL_NO_ERROR,"effect regressions leave no GL error");
    gl::UseProgram(0); gl::DeleteProgram(program); gl::DeleteShader(vs); gl::DeleteShader(fs);
    glDeleteTextures(1,&texture); gl::BindVertexArray(0); gl::DeleteVertexArrays(1,&vao);
    EnhancedEffectsShutdown(); testState={}; testShaders[0]={};
}
