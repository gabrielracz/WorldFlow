#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vk_types.h>

typedef uint32_t AssetHandle;

class Transform
{
    glm::vec3 position;
    glm::quat orientation;
    glm::vec3 scale;
};

class Shader
{

};

class Texture
{
    AssetHandle _image;
};

class Material
{
    AssetHandle _shader;
    AssetHandle _texture;
    AssetHandle _normalMap;
};

class GameObjectComponent
{
    Transform _transform;
    Material _material;
    AssetHandle _mesh;
};

class Mesh
{
    std::vector<Vertex> vertexBuffer;
    std::vector<uint32_t> indexBuffer;
};

class AssetManager
{
    std::unordered_map<AssetHandle, Mesh> _meshes;
    std::unordered_map<AssetHandle, Shader> _shaders;
    std::unordered_map<AssetHandle, AllocatedImage> _images;

}