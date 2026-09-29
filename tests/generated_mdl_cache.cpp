#include "../src/materialx/generated_cache.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using prism::materialx::GeneratedTemplateArtifact;
using prism::materialx::GeneratedTemplateDiskCache;

void require(bool condition,char const* message) {
    if(!condition)throw std::runtime_error(message);
}

GeneratedTemplateArtifact fixture() {
    GeneratedTemplateArtifact artifact;
    artifact.moduleName="::materialx::test";
    artifact.source="export material test(color c = color(0.000000, 0.000000, 0.000000)) = material();";
    artifact.texturePaths={"texture.exr"};
    const auto begin=artifact.source.find("color(");
    const auto end=artifact.source.find(')',begin)+1;
    artifact.spans={{"Surface/base_color",{begin,end}}};
    artifact.uniforms.push_back({"Surface_base_color",
        "Surface/base_color", "color3"});
    return artifact;
}

int main(int argc,char** argv) {
    try {
        if(argc==3&&std::string(argv[1])=="--writer") {
            const auto artifact=fixture();
            const auto key=prism::materialx::GeneratedTemplateKey(
                {"concurrent-process-fixture"});
            GeneratedTemplateDiskCache cache(argv[2],1024*1024);
            require(cache.store(key,artifact),"process writer failed to publish");
            require(cache.load(key).has_value(),"process writer cannot reload winner");
            return 0;
        }
        const auto root=std::filesystem::temp_directory_path()/
            ("prism-generated-cache-test-"+std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root/"library");
        const auto library=root/"library"/"definition.mtlx";
        {std::ofstream out(library);out<<"first-content";}
        const auto timestamp=std::filesystem::last_write_time(library);
        const auto digestA=prism::materialx::MaterialXLibraryDigest(root/"library");
        {std::ofstream out(library,std::ios::trunc);out<<"other-content";}
        std::filesystem::last_write_time(library,timestamp);
        const auto digestB=prism::materialx::MaterialXLibraryDigest(root/"library");
        require(digestA!=digestB,"library digest trusted size/timestamp instead of contents");

        const auto artifact=fixture();
        const auto key=prism::materialx::GeneratedTemplateKey({"recipe",digestB,"graph"});
        GeneratedTemplateDiskCache cache(root,1024*1024);
        require(cache.store(key,artifact),"cannot store generated artifact");
        const auto loaded=cache.load(key);
        require(loaded&&loaded->source==artifact.source&&loaded->spans==artifact.spans&&
                    loaded->uniforms.size()==1&&
                    loaded->uniforms.front().name==artifact.uniforms.front().name&&
                    loaded->uniforms.front().path==artifact.uniforms.front().path&&
                    loaded->uniforms.front().type==artifact.uniforms.front().type,
                "generated artifact did not round-trip");

        const auto path=cache.pathForTesting(key);
        {std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);
         file.seekp(32);char value=0;file.write(&value,1);}
        require(!cache.load(key),"corrupted generated artifact was accepted");

        require(cache.store(key,artifact),"corrupted artifact was not replaced safely");
        const auto artifactBytes=std::filesystem::file_size(path);
        {std::filesystem::resize_file(path,artifactBytes/2);}
        require(!cache.load(key),"truncated generated artifact was accepted");

        auto badSpan=artifact;
        badSpan.spans.front().second.second=badSpan.source.size()+1;
        require(!cache.store(prism::materialx::GeneratedTemplateKey({"bad-span"}),badSpan),
                "out-of-range generated span was accepted");

        GeneratedTemplateDiskCache exact(root/"exact",artifactBytes);
        require(exact.store(key,artifact),"artifact equal to quota was rejected");
        GeneratedTemplateDiskCache oneByteShort(root/"short",artifactBytes-1);
        require(!oneByteShort.store(key,artifact),"artifact larger than quota was published");

        GeneratedTemplateDiskCache concurrent(root/"concurrent",1024*1024);
        std::vector<std::thread> writers;
        std::vector<int> results(8);
        for(size_t i=0;i<results.size();++i)
            writers.emplace_back([&,i]{results[i]=concurrent.store(key,artifact);});
        for(auto& writer:writers)writer.join();
        for(bool result:results)require(result,"concurrent artifact publication failed");
        require(concurrent.load(key).has_value(),"concurrent publication left no valid artifact");

        GeneratedTemplateDiskCache tiny(root/"tiny",32);
        require(!tiny.store(key,artifact),"oversized artifact ignored zero/tiny allowance");
        std::error_code ec;std::filesystem::remove_all(root,ec);
        return 0;
    } catch(std::exception const& error) {
        std::cerr<<error.what()<<'\n';return 1;
    }
}
