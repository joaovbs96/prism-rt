#pragma once
// Historical asset-specific fixtures. Ordinary rendering uses the CLI.
// These are retained for reproducing experiments, not compiled into PrismRT.
#include "../src/scene.h"
#include <cstdlib>
inline std::string mx_libraries_path = std::getenv("PRISM_MATERIALX_LIBRARY_PATH") ? std::getenv("PRISM_MATERIALX_LIBRARY_PATH") : "";
inline std::string mx_mdl_path = mx_libraries_path + "/mdl";

class Earth : public SceneManager
{
public:
    Earth(RTCDevice device_, unsigned int &seed) : SceneManager(device_)
    {
        add_sphere(Float3(0, 0, 0), 2.0f, new Diffuse(new ImageTexture(getAssetsPath() / "textures" / "earthmap.jpg")));

        std::cout << "Finished geometry creation." << std::endl;

        // camera initialization
        Float3 position(0, 0, 12);
        Float3 target(0.0f);
        Float3 up(0.0f, 1.0f, 0.0f);
        float vfov = 20;
        cam = Camera(position, target, up, 1280, 720, 16.0f / 9.0f, vfov, 0.0f, 10.0f);

        // environment map - for missed rays
        environment = new GradientBackground(new ColorTexture(Color3(1.0, 1.0, 1.0)), new ColorTexture(Color3(0.5, 0.7, 1.0)));
    }
};

class ABeautifulGame : public SceneManager
{
public:
    ABeautifulGame(RTCDevice device_, unsigned int &seed) : SceneManager(device_)
    {
        std::cout << "Started scene initialization." << std::endl;
        add_mesh(getAssetsPath() / "models" / "ABeautifulGame" / "ABeautifulGame.gltf");

        std::cout << "Finished geometry creation." << std::endl;

        // camera initialization
        Float3 position(0.8, 0.15, 0.7);
        Float3 target(0.0f, 0.05, 0.0);
        Float3 up(0.0f, 1.0f, 0.0f);
        float vfov = 12;
        cam = Camera(position, target, up, 1280, 720, 16.0f / 9.0f, vfov, 0.0f, 10.0f);

        // environment map - for missed rays
        ImageTexture<Float3> *texture = new ImageTexture<Float3>(getAssetsPath() / "environment" / "IndoorEnvironmentHDRI003_4K-HDR.exr");
        texture->adjustExposure(0.5f);
        environment = new EnvironmentMap(texture);
    }
};

class Duck : public SceneManager
{
public:
    Duck(RTCDevice device_, unsigned int &seed) : SceneManager(device_)
    {
        std::cout << "Started scene initialization." << std::endl;
        add_mesh(getAssetsPath() / "models" / "Duck" / "Duck.glb");

        std::cout << "Finished geometry creation." << std::endl;

        // camera initialization
        Float3 position(11, 2, 7.5);
        Float3 target(0.0f, 0.85, 0.0);
        Float3 up(0.0f, 1.0f, 0.0f);
        float vfov = 8;
        cam = Camera(position, target, up, 1280, 720, 16.0f / 9.0f, vfov, 0.0f, 10.0f);

        // environment map - for missed rays
        environment = new GradientBackground(new ColorTexture(Color3(1.0, 1.0, 1.0)), new ColorTexture(Color3(0.5, 0.7, 1.0)));
    }
};

class PixarKitchen : public SceneManager
{
public:
    PixarKitchen(RTCDevice device_, unsigned int &seed) : SceneManager(device_)
    {
        std::cout << "Started scene initialization." << std::endl;
        add_mesh(getAssetsPath() / "models" / "Kitchen_set" / "Kitchen_set.usd");

        std::cout << "Finished geometry creation." << std::endl;

        // camera initialization
        Float3 position(75, -385, 120);
        Float3 target(75, 0.0f, 120);
        Float3 up(0.0f, 0.0f, 1.0f);
        float vfov = 60;
        cam = Camera(position, target, up, 1280, 720, 16.0f / 9.0f, vfov, 0.f, 10.0f);

        // environment map - for missed rays
        environment = new ConstantColorBackground(new ColorTexture(Color3(1.0f)));
    }
};

class InOneWeekendFinalDispersion : public SceneManager {
    public:
        InOneWeekendFinalDispersion(RTCDevice device_, unsigned int &seed) : SceneManager(device_) {
            std::cout << "Started scene initialization." << std::endl;
            add_sphere(Float3(0, -1000, 0), 1000, new Diffuse(new ColorTexture(Color3(0.5f))));

            auto diffuse_light = new Emissive(new ColorTexture(Color3(4.0f)));
            add_sphere(Float3(0, 7, 0), 2.0f, diffuse_light);
            add_quad(Float3(3,1,-2), Float3(2,0,0), Float3(0,2,0), diffuse_light);

            // add_sphere(Float3(4, 1, 0), 1.0f, new Dispersive(2.417f, 55.0f));
            auto mesh = add_mesh(getAssetsPath() / "models" / "diamond" / "diamond.glb", new Dispersive(2.417f, 55.0f));
            // mesh->setPosition(Float3(4, 2, 0));
            mesh->transform->translate(0, 0.6, 0);
            mesh->transform->scale(100, 100, 100);

            std::cout << "Finished geometry creation." << std::endl;

            // camera initialization
            Float3 position(13, 2, 3);
            Float3 target(0.0f);
            Float3 up(0.0f, 1.0f, 0.0f);
            float vfov = 20;
            cam = Camera(position, target, up, 1280, 720, 16.0f / 9.0f, vfov, 0.6f, 10.0f);

            // environment map - for missed rays
            environment = new ConstantColorBackground(new ColorTexture(Color3(0.0, 0.0, 0.0)));
            // environment = new GradientBackground(new ColorTexture(Color3(1.0, 1.0, 1.0)), new ColorTexture(Color3(0.5, 0.7, 1.0)));
        }
};

class MaterialXTest : public SceneManager
{
public:
    MaterialXTest(RTCDevice device_, unsigned int& seed, mdl::Context& mdl_context)
        : SceneManager(device_)
    {
        std::cout << "Started MaterialX test scene initialization." << std::endl;

        // Build the full path: assets/materials/<filename>
        std::string mtlx_path = (std::getenv("PRISM_TEST_MATERIAL_FILE") ? std::getenv("PRISM_TEST_MATERIAL_FILE") : "");

        // Create MDL material — self-contained compilation pipeline
        // Empty material_name = auto-detect first material in file
        MDLMaterial* mdl_mat = new MDLMaterial(mtlx_path, mdl_context, mx_libraries_path, mx_mdl_path);

        if (!mdl_mat->is_valid()) {
            std::cerr << "[MaterialXTest] WARNING: MDL material failed, using magenta error shading\n";
        }

        // Ground plane
        add_sphere(Float3(0, -1000, 0), 1000, new Diffuse(new ColorTexture(Color3(0.5f))));

        // Main sphere with the MaterialX material
        add_sphere(Float3(0, 1, 0), 1.0f, mdl_mat);

        std::cout << "Finished geometry creation." << std::endl;

        // Camera — looking at the center sphere
        Float3 position(0, 2, 8);
        Float3 target(0, 1, 0);
        Float3 up(0.0f, 1.0f, 0.0f);
        float vfov = 30;
        cam = Camera(position, target, up, 1280, 720, 16.0f / 9.0f, vfov, 0.0f, 10.0f);

        // Environment — gradient sky
        environment = new GradientBackground(
            new ColorTexture(Color3(1.0, 1.0, 1.0)),
            new ColorTexture(Color3(0.5, 0.7, 1.0)));
    }
};

class ABeautifulGameMtlX : public SceneManager
{
public:
    ABeautifulGameMtlX(RTCDevice device_, unsigned int &seed, mdl::Context& mdl_context) : SceneManager(device_)
    {
        std::cout << "Started scene initialization." << std::endl;
        add_mesh(getAssetsPath() / "models" / "OpenChessSet" / "chess_set.usda", nullptr, &mdl_context, mx_libraries_path, mx_mdl_path);

        std::cout << "Finished geometry creation." << std::endl;

        // camera initialization
        Float3 position(0.8, 0.15, 0.7);
        Float3 target(0.0f, 0.05, 0.0);
        Float3 up(0.0f, 1.0f, 0.0f);
        float vfov = 12;
        cam = Camera(position, target, up, 1280, 720, 16.0f / 9.0f, vfov, 0.0f, 10.0f);

        // // environment map - for missed rays
        ImageTexture<Float3> *texture = new ImageTexture<Float3>(getAssetsPath() / "environment" / "IndoorEnvironmentHDRI003_4K-HDR.exr");
        texture->adjustExposure(-0.5f);
        environment = new EnvironmentMap(texture);
        // Environment — gradient sky
        // environment = new GradientBackground(
        //     new ColorTexture(Color3(1.0, 1.0, 1.0)),
        //     new ColorTexture(Color3(0.5, 0.7, 1.0)));
    }
};
