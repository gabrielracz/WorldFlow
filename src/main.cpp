// #define VMA_IMPLEMENTATION
// #include "vk_mem_alloc.h"
#include "vma.hpp"
#include "fluid_engine.hpp"
#include "uniform_fluid_engine.hpp"
#include "renderer.hpp"
#include "defines.hpp"

int main(int argc, char* argv[])
{
    Renderer renderer("Aether", 900, 900);
    if(!renderer.Init()) {
        std::cout << "[ERROR] Failed to initialize renderer" << std::endl;
        return EXIT_FAILURE;
    }

	UniformFluidEngine fluidEngine(renderer);
	if(!fluidEngine.Init()) {
        std::cout << "[ERROR] Failed to initialize fluid engine" << std::endl;
		return EXIT_FAILURE;
	}

    const double startTime = GetTimestamp();
    double lastFrameTime = startTime;
    while(!renderer.ShouldClose()) {
        const double currentFrameTime = GetTimestamp();
        const double dt = currentFrameTime - lastFrameTime;
        lastFrameTime = currentFrameTime;
        renderer.Update((float)dt);
    }

    std::cout << "Closed" << std::endl;
    return EXIT_SUCCESS;
}