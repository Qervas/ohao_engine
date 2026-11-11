#include "asset/cooked.hpp"

#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ohao::cooked {

namespace {

constexpr char kModelMagic[8] = {'O', 'H', 'A', 'O', 'M', 'D', 'L', '\0'};
constexpr char kTextureMagic[8] = {'O', 'H', 'A', 'O', 'T', 'E', 'X', '\0'};

struct Header {
    char magic[8];
    uint32_t version;
    uint32_t count;   // a model's meshes; a texture's levels
    uint64_t size;    // the whole file
};

// Offsets are from the file's start; every array starts on 8 bytes.
struct MeshRecord {
    uint64_t name, nameSize, error, errorSize;
    uint64_t vertices, vertexCount, indices, indexCount, slots, slotCount, materials, materialCount;
    uint64_t bones, bonesSize, boneCount;   // names, each ended by a zero
};

struct TextureRecord {
    uint32_t width, height;
    uint64_t pixels, pixelsSize;
};

class Writer {
public:
    uint64_t reserve(std::size_t size) {
        pad();
        const uint64_t at = m_bytes.size();
        m_bytes.resize(m_bytes.size() + size);
        return at;
    }
    uint64_t add(const void* data, std::size_t size) {
        const uint64_t at = reserve(size);
        if (size) std::memcpy(m_bytes.data() + at, data, size);
        return at;
    }
    template <class T> uint64_t add(std::span<const T> v) { return add(v.data(), v.size_bytes()); }
    template <class T> void put(uint64_t at, const T& v) { std::memcpy(m_bytes.data() + at, &v, sizeof v); }
    [[nodiscard]] std::size_t size() const noexcept { return m_bytes.size(); }

    [[nodiscard]] VoidResult<> write(const std::filesystem::path& path) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        const std::filesystem::path temp = path.string() + "." + std::to_string(::getpid()) + ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(m_bytes.data()), static_cast<std::streamsize>(m_bytes.size()));
            if (!out) return err_void("cannot write '" + temp.string() + "'");
        }
        std::filesystem::rename(temp, path, ec);
        if (ec) {
            std::filesystem::remove(temp, ec);
            return err_void("cannot write '" + path.string() + "'");
        }
        return VoidResult<>::ok();
    }

private:
    void pad() { m_bytes.resize((m_bytes.size() + 7) & ~std::size_t(7)); }
    std::vector<uint8_t> m_bytes;
};

// The header of a cooked file of this kind and version, whole.
const Header* headerOf(std::span<const uint8_t> bytes, const char* magic) {
    if (bytes.size() < sizeof(Header)) return nullptr;
    const auto* h = reinterpret_cast<const Header*>(bytes.data());
    if (std::memcmp(h->magic, magic, 8) != 0 || h->version != kFormatVersion || h->size != bytes.size()) return nullptr;
    return h;
}

// [at, at + count * size) lies inside the file.
bool inside(std::span<const uint8_t> bytes, uint64_t at, uint64_t count, uint64_t size) {
    return at <= bytes.size() && (size == 0 || count <= (bytes.size() - at) / size) && at % 8 == 0;
}

template <class T> std::span<const T> arrayAt(std::span<const uint8_t> bytes, uint64_t at, uint64_t count) {
    return {reinterpret_cast<const T*>(bytes.data() + at), static_cast<std::size_t>(count)};
}

} // namespace

Result<std::shared_ptr<const Mapping>> Mapping::open(const std::filesystem::path& path) {
    using R = Result<std::shared_ptr<const Mapping>>;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return R::err("cannot open '" + path.string() + "'");
    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        return R::err("'" + path.string() + "' is empty");
    }
    void* p = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) return R::err("cannot map '" + path.string() + "'");
    std::shared_ptr<Mapping> m(new Mapping());
    m->m_data = static_cast<const uint8_t*>(p);
    m->m_size = static_cast<std::size_t>(st.st_size);
    return R::ok(std::move(m));
}

Mapping::~Mapping() {
    if (m_data) ::munmap(const_cast<uint8_t*>(m_data), m_size);
}

VoidResult<> writeModel(const std::filesystem::path& path, std::span<const MeshSource> meshes) {
    Writer w;
    w.reserve(sizeof(Header));
    const uint64_t records = w.reserve(sizeof(MeshRecord) * meshes.size());
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        const MeshSource& m = meshes[i];
        MeshRecord r{};
        r.name = w.add(m.name.data(), m.name.size());
        r.nameSize = m.name.size();
        r.error = w.add(m.error.data(), m.error.size());
        r.errorSize = m.error.size();
        r.vertices = w.add(m.vertices);
        r.vertexCount = m.vertices.size();
        r.indices = w.add(m.indices);
        r.indexCount = m.indices.size();
        r.slots = w.add(m.materialOfTriangle);
        r.slotCount = m.materialOfTriangle.size();
        r.materials = w.add(std::span<const Material>(m.materials));
        r.materialCount = m.materials.size();
        std::string names;
        for (const std::string& b : m.bones) names.append(b).push_back('\0');
        r.bones = w.add(names.data(), names.size());
        r.bonesSize = names.size();
        r.boneCount = m.bones.size();
        w.put(records + sizeof(MeshRecord) * i, r);
    }
    Header h{};
    std::memcpy(h.magic, kModelMagic, 8);
    h.version = kFormatVersion;
    h.count = static_cast<uint32_t>(meshes.size());
    h.size = w.size();
    w.put(0, h);
    return w.write(path);
}

Result<std::shared_ptr<const ModelFile>> ModelFile::open(const std::filesystem::path& path, const ContentHash& key) {
    using R = Result<std::shared_ptr<const ModelFile>>;
    auto map = Mapping::open(path);
    if (!map) return R::err(map.error());
    const std::span<const uint8_t> bytes = map.value()->bytes();
    const std::string bad = "'" + path.string() + "' is not a cooked model of this version";
    const Header* h = headerOf(bytes, kModelMagic);
    if (!h || !inside(bytes, sizeof(Header), h->count, sizeof(MeshRecord))) return R::err(bad);
    auto file = std::make_shared<ModelFile>();
    file->m_map = map.value();
    const auto records = arrayAt<MeshRecord>(bytes, sizeof(Header), h->count);
    for (const MeshRecord& r : records) {
        if (!inside(bytes, r.name, r.nameSize, 1) || !inside(bytes, r.error, r.errorSize, 1) ||
            !inside(bytes, r.vertices, r.vertexCount, sizeof(Vertex)) || !inside(bytes, r.indices, r.indexCount, 4) ||
            !inside(bytes, r.slots, r.slotCount, 4) || !inside(bytes, r.materials, r.materialCount, sizeof(Material)) ||
            !inside(bytes, r.bones, r.bonesSize, 1))
            return R::err(bad);
        Mesh m;
        ContentHasher id;
        id.value(key.hi);
        id.value(key.lo);
        id.value(static_cast<uint64_t>(file->m_meshes.size()));
        m.id = id.finish();
        const char* text = reinterpret_cast<const char*>(bytes.data());
        m.name = {text + r.name, static_cast<std::size_t>(r.nameSize)};
        m.error = {text + r.error, static_cast<std::size_t>(r.errorSize)};
        m.vertices = arrayAt<Vertex>(bytes, r.vertices, r.vertexCount);
        m.indices = arrayAt<uint32_t>(bytes, r.indices, r.indexCount);
        m.materialOfTriangle = arrayAt<uint32_t>(bytes, r.slots, r.slotCount);
        m.materials = arrayAt<Material>(bytes, r.materials, r.materialCount);
        std::string_view names{text + r.bones, static_cast<std::size_t>(r.bonesSize)};
        for (uint64_t b = 0; b < r.boneCount; ++b) {
            const std::size_t end = names.find('\0');
            if (end == std::string_view::npos) return R::err(bad);
            m.bones.push_back(names.substr(0, end));
            names.remove_prefix(end + 1);
        }
        file->m_meshes.push_back(std::move(m));
    }
    return R::ok(std::move(file));
}

Result<std::size_t> ModelFile::find(std::string_view path, std::string_view mesh) const {
    using R = Result<std::size_t>;
    const std::string where = "'" + std::string(path) + "'";
    const int count = static_cast<int>(m_meshes.size());
    int index = -1;
    if (!mesh.empty() && mesh.front() == '#') {
        int value = 0;
        const auto [end, ec] = std::from_chars(mesh.data() + 1, mesh.data() + mesh.size(), value);
        if (ec == std::errc() && end == mesh.data() + mesh.size() && value >= 0 && value < count) index = value;
    } else {
        for (int i = 0; i < count; ++i) {
            if (m_meshes[static_cast<std::size_t>(i)].name != mesh) continue;
            if (index >= 0)
                return R::err(where + ": meshes " + std::to_string(index) + " and " + std::to_string(i) + " are both named '" +
                              std::string(mesh) + "'; use '#" + std::to_string(index) + "'");
            index = i;
        }
    }
    if (index < 0)
        return R::err(where + " has no mesh '" + std::string(mesh) + "' (" + std::to_string(count) + " meshes)");
    return R::ok(static_cast<std::size_t>(index));
}

VoidResult<> writeFile(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    Writer w;
    (void)w.add(bytes);
    return w.write(path);
}

VoidResult<> writeTexture(const std::filesystem::path& path, uint32_t width, uint32_t height,
                          std::span<const uint8_t> rgba8) {
    if (rgba8.size() != std::size_t(width) * height * 4) return err_void("a texture's pixels are not width x height x 4");
    Writer w;
    w.reserve(sizeof(Header));
    const uint64_t record = w.reserve(sizeof(TextureRecord));
    TextureRecord r{};
    r.width = width;
    r.height = height;
    r.pixels = w.add(rgba8);
    r.pixelsSize = rgba8.size();
    w.put(record, r);
    Header h{};
    std::memcpy(h.magic, kTextureMagic, 8);
    h.version = kFormatVersion;
    h.count = 1;
    h.size = w.size();
    w.put(0, h);
    return w.write(path);
}

Result<Texture> openTexture(const std::filesystem::path& path) {
    using R = Result<Texture>;
    auto map = Mapping::open(path);
    if (!map) return R::err(map.error());
    const std::span<const uint8_t> bytes = map.value()->bytes();
    const std::string bad = "'" + path.string() + "' is not a cooked texture of this version";
    const Header* h = headerOf(bytes, kTextureMagic);
    if (!h || h->count != 1 || !inside(bytes, sizeof(Header), 1, sizeof(TextureRecord))) return R::err(bad);
    const TextureRecord& r = arrayAt<TextureRecord>(bytes, sizeof(Header), 1)[0];
    if (!inside(bytes, r.pixels, r.pixelsSize, 1) || r.pixelsSize != uint64_t(r.width) * r.height * 4) return R::err(bad);
    Texture t;
    t.map = map.value();
    t.width = r.width;
    t.height = r.height;
    t.pixels = arrayAt<uint8_t>(bytes, r.pixels, r.pixelsSize);
    return R::ok(std::move(t));
}

} // namespace ohao::cooked
