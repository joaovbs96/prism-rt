#include "generated_cache.h"

#include <OpenImageIO/hash.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace prism::materialx {
namespace {
constexpr std::array<char,8> magic{'P','R','M','D','L','G','1','\0'};
constexpr uint32_t formatVersion=2;
constexpr uint64_t maxArtifactBytes=16ull*1024*1024;
constexpr uint32_t maxStrings=4096;
std::array<std::mutex,64> publicationMutexes;
std::mutex pruneMutex;

class CrossProcessGeneratedCacheLock {
public:
    explicit CrossProcessGeneratedCacheLock(std::filesystem::path const& root) {
#ifdef _WIN32
        const auto path=std::filesystem::absolute(root).lexically_normal().string();
        const auto digest=OIIO::SHA1::digest(path.data(),path.size());
        const std::wstring name=L"Local\\PrismGeneratedCache-"+
            std::wstring(digest.begin(),digest.end());
        handle=CreateMutexW(nullptr,FALSE,name.c_str());
        if(!handle)throw std::runtime_error("cannot create generated cache mutex");
        const DWORD wait=WaitForSingleObject(handle,INFINITE);
        if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED){CloseHandle(handle);handle=nullptr;
            throw std::runtime_error("cannot acquire generated cache mutex");}
#else
        const auto path=root/".prism-generated-cache.lock";
        fd=open(path.c_str(),O_CREAT|O_RDWR,0666);
        if(fd<0||flock(fd,LOCK_EX)!=0){if(fd>=0)close(fd);fd=-1;
            throw std::runtime_error("cannot acquire generated cache lock");}
#endif
    }
    ~CrossProcessGeneratedCacheLock(){
#ifdef _WIN32
        if(handle){ReleaseMutex(handle);CloseHandle(handle);}
#else
        if(fd>=0){flock(fd,LOCK_UN);close(fd);}
#endif
    }
private:
#ifdef _WIN32
    HANDLE handle=nullptr;
#else
    int fd=-1;
#endif
};

bool AtomicReplace(std::filesystem::path const& from,
                   std::filesystem::path const& to) {
#ifdef _WIN32
    return MoveFileExW(from.c_str(),to.c_str(),
        MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
    std::error_code ec;std::filesystem::rename(from,to,ec);return !ec;
#endif
}

void hashField(OIIO::SHA1& hash,std::string_view value) {
    const uint64_t length=value.size();
    hash.append(&length,sizeof(length));
    hash.append(value.data(),value.size());
}
template<class T> bool read(std::istream& in,T& value) {
    return bool(in.read(reinterpret_cast<char*>(&value),sizeof(value)));
}
template<class T> void write(std::ostream& out,T value) {
    out.write(reinterpret_cast<char const*>(&value),sizeof(value));
}
bool readString(std::istream& in,std::string& value,uint64_t& remaining) {
    uint64_t size=0;
    if(!read(in,size)||size>remaining||size>maxArtifactBytes)return false;
    value.resize(static_cast<size_t>(size));
    if(size&&!in.read(value.data(),static_cast<std::streamsize>(size)))return false;
    remaining-=size; return true;
}
void writeString(std::ostream& out,std::string const& value) {
    write(out,uint64_t(value.size()));
    out.write(value.data(),static_cast<std::streamsize>(value.size()));
}
bool validKey(std::string const& key) {
    return key.size()==40&&std::all_of(key.begin(),key.end(),[](unsigned char c){
        return (c>='0'&&c<='9')||(c>='a'&&c<='f'); });
}
std::string artifactChecksum(std::string const& key,
                            GeneratedTemplateArtifact const& value) {
    std::vector<std::string> fields{key,value.moduleName,value.source};
    fields.insert(fields.end(),value.texturePaths.begin(),value.texturePaths.end());
    for(auto const& span:value.spans) {
        fields.push_back(span.first);
        fields.push_back(std::to_string(span.second.first));
        fields.push_back(std::to_string(span.second.second));
    }
    for(auto const& uniform:value.uniforms) {
        fields.push_back(uniform.name); fields.push_back(uniform.path);
        fields.push_back(uniform.type);
    }
    return GeneratedTemplateKey(fields);
}
}

std::string GeneratedTemplateKey(std::vector<std::string> const& fields) {
    OIIO::SHA1 hash;
    for(auto const& field:fields) hashField(hash,field);
    auto digest=hash.digest();
    std::transform(digest.begin(),digest.end(),digest.begin(),[](unsigned char c){
        return static_cast<char>(std::tolower(c)); });
    return digest;
}

std::string MaterialXLibraryDigest(std::filesystem::path const& root) {
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for(std::filesystem::recursive_directory_iterator it(
            root,std::filesystem::directory_options::none,ec),end;
        !ec&&it!=end;it.increment(ec))
        if(it->is_regular_file(ec)) files.push_back(it->path());
    if(ec)return {};
    std::sort(files.begin(),files.end(),[&](auto const& a,auto const& b){
        return a.lexically_relative(root).generic_string()<
               b.lexically_relative(root).generic_string(); });
    OIIO::SHA1 hash;
    hashField(hash,"prism-materialx-library-manifest-v1");
    std::array<char,64*1024> buffer{};
    for(auto const& file:files) {
        hashField(hash,file.lexically_relative(root).generic_string());
        std::ifstream in(file,std::ios::binary);
        if(!in)return {};
        OIIO::SHA1 content;
        while(in) {
            in.read(buffer.data(),buffer.size());
            const auto count=in.gcount();
            if(count>0) content.append(buffer.data(),static_cast<size_t>(count));
        }
        if(in.bad())return {};
        hashField(hash,content.digest());
    }
    return hash.digest();
}

GeneratedTemplateDiskCache::GeneratedTemplateDiskCache(
    std::filesystem::path root,uint64_t budgetBytes)
    :m_root(std::move(root)/"generated-mdl"),m_budgetBytes(budgetBytes) {
    std::error_code ec;std::filesystem::create_directories(m_root,ec);
    if(!ec)prune();
}

std::filesystem::path GeneratedTemplateDiskCache::pathForTesting(
    std::string const& key) const {
    if(!m_budgetBytes)return {};
    if(!validKey(key))return {};
    return m_root/key.substr(0,2)/(key+".bin");
}

std::optional<GeneratedTemplateArtifact> GeneratedTemplateDiskCache::load(
    std::string const& key) const {
    const auto path=pathForTesting(key); if(path.empty())return {};
    std::error_code ec;
    const auto bytes=std::filesystem::file_size(path,ec);
    if(ec||bytes<magic.size()+sizeof(uint32_t)+40||bytes>maxArtifactBytes)return {};
    std::ifstream in(path,std::ios::binary); if(!in)return {};
    std::array<char,8> found{}; uint32_t version=0;
    if(!in.read(found.data(),found.size())||found!=magic||!read(in,version)||
       version!=formatVersion)return {};
    std::string storedKey; uint64_t remaining=bytes-magic.size()-sizeof(version);
    if(!readString(in,storedKey,remaining)||storedKey!=key)return {};
    GeneratedTemplateArtifact value;
    if(!readString(in,value.moduleName,remaining)||
       !readString(in,value.source,remaining))return {};
    uint32_t textures=0,spans=0;
    if(!read(in,textures)||textures>maxStrings)return {};
    for(uint32_t i=0;i<textures;++i) {
        std::string item; if(!readString(in,item,remaining))return {};
        value.texturePaths.push_back(std::move(item));
    }
    if(!read(in,spans)||spans>maxStrings)return {};
    size_t previous=value.source.size()+1;
    for(uint32_t i=0;i<spans;++i) {
        std::string name; uint64_t begin=0,end=0;
        if(!readString(in,name,remaining)||!read(in,begin)||!read(in,end)||
           begin>=end||end>value.source.size()||end>previous||
           value.source.compare(static_cast<size_t>(begin),6,"color(")!=0)return {};
        previous=static_cast<size_t>(begin);
        value.spans.push_back({std::move(name),{static_cast<size_t>(begin),
                                                static_cast<size_t>(end)}});
    }
    uint32_t uniforms=0;
    if(!read(in,uniforms)||uniforms>maxStrings)return {};
    for(uint32_t i=0;i<uniforms;++i) {
        GeneratedTemplateArtifact::Uniform uniform;
        if(!readString(in,uniform.name,remaining)||
           !readString(in,uniform.path,remaining)||
           !readString(in,uniform.type,remaining))return {};
        value.uniforms.push_back(std::move(uniform));
    }
    std::string checksum;
    if(!readString(in,checksum,remaining)||checksum.size()!=40)return {};
    const auto actual=artifactChecksum(storedKey,value);
    if(checksum!=actual||in.peek()!=std::char_traits<char>::eof())return {};
    std::filesystem::last_write_time(path,std::filesystem::file_time_type::clock::now(),ec);
    return value;
}

bool GeneratedTemplateDiskCache::store(
    std::string const& key,GeneratedTemplateArtifact const& value) const {
    const auto path=pathForTesting(key); if(path.empty()||!m_budgetBytes)return false;
    // Serialize same-process writers of one immutable key. Cross-process
    // writers still converge through close-then-rename publication below.
    std::lock_guard publicationLock(
        publicationMutexes[std::hash<std::string>{}(key)%publicationMutexes.size()]);
    uint64_t estimate=132+value.moduleName.size()+value.source.size();
    for(auto const& item:value.texturePaths)estimate+=8+item.size();
    for(auto const& span:value.spans)estimate+=24+span.first.size();
    for(auto const& uniform:value.uniforms)
        estimate+=24+uniform.name.size()+uniform.path.size()+uniform.type.size();
    if(estimate>maxArtifactBytes||estimate>m_budgetBytes||
       value.texturePaths.size()>maxStrings||value.spans.size()>maxStrings||
       value.uniforms.size()>maxStrings)return false;
    size_t previous=value.source.size()+1;
    for(auto const& span:value.spans) {
        const auto [begin,end]=span.second;
        if(begin>=end||end>value.source.size()||end>previous||
           value.source.compare(begin,6,"color(")!=0)return false;
        previous=begin;
    }
    std::error_code ec; std::filesystem::create_directories(path.parent_path(),ec);
    if(ec)return false;
    CrossProcessGeneratedCacheLock processLock(m_root);
    // A writer that won while this process waited published the same immutable
    // content. Avoid touching its recency or entering a replacement path.
    if(load(key))return true;
    const auto nonce=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
        "."+std::to_string(std::random_device{}());
    const auto temp=path.string()+".tmp."+nonce;
    bool wrote=false;
    {
        std::ofstream out(temp,std::ios::binary|std::ios::trunc); if(!out)return false;
        out.write(magic.data(),magic.size()); write(out,formatVersion);
        writeString(out,key); writeString(out,value.moduleName);writeString(out,value.source);
        write(out,uint32_t(value.texturePaths.size()));
        for(auto const& item:value.texturePaths)writeString(out,item);
        write(out,uint32_t(value.spans.size()));
        for(auto const& span:value.spans) {
            writeString(out,span.first);write(out,uint64_t(span.second.first));
            write(out,uint64_t(span.second.second));
        }
        write(out,uint32_t(value.uniforms.size()));
        for(auto const& uniform:value.uniforms) {
            writeString(out,uniform.name);writeString(out,uniform.path);
            writeString(out,uniform.type);
        }
        writeString(out,artifactChecksum(key,value));
        out.flush(); wrote=bool(out);
    }
    if(!wrote){std::filesystem::remove(temp,ec);return false;}
    const auto actualBytes=std::filesystem::file_size(temp,ec);
    if(ec||actualBytes>maxArtifactBytes||actualBytes>m_budgetBytes) {
        std::filesystem::remove(temp,ec);
        return false;
    }
    if(!AtomicReplace(temp,path)){
        std::filesystem::remove(temp,ec);return false;
    }
    pruneUnlocked(); return true;
}

void GeneratedTemplateDiskCache::prune() const {
    CrossProcessGeneratedCacheLock processLock(m_root);
    pruneUnlocked();
}

void GeneratedTemplateDiskCache::pruneUnlocked() const {
    std::lock_guard pruneLock(pruneMutex);
    std::error_code ec; uint64_t total=0;
    struct File {std::filesystem::path path;uint64_t size;std::filesystem::file_time_type time;};
    std::vector<File> files;
    const auto stale=std::filesystem::file_time_type::clock::now()-std::chrono::hours(1);
    for(std::filesystem::recursive_directory_iterator it(
            m_root,std::filesystem::directory_options::skip_permission_denied,ec),end;
        !ec&&it!=end;it.increment(ec)) if(it->is_regular_file(ec)) {
            const auto size=it->file_size(ec); if(ec){ec.clear();continue;}
            const auto time=it->last_write_time(ec);if(ec){ec.clear();continue;}
            const auto name=it->path().filename().string();
            if(name==".prism-generated-cache.lock")continue;
            const bool temporary=name.find(".tmp.")!=std::string::npos||
                                 name.find(".bad.")!=std::string::npos;
            if(temporary) {
                if(time<stale)std::filesystem::remove(it->path(),ec);
                ec.clear();continue;
            }
            files.push_back({it->path(),size,time});total+=size;
        }
    std::sort(files.begin(),files.end(),[](auto const& a,auto const& b){return a.time<b.time;});
    for(auto const& file:files) if(total>m_budgetBytes) {
        std::filesystem::remove(file.path,ec); if(!ec)total-=file.size;ec.clear();
    }
}
} // namespace prism::materialx
