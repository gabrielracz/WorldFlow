// #define VMA_IMPLEMENTATION
// #include "vk_mem_alloc.h"
#include "vma.hpp"
#include "renderer.hpp"
#include "vine_generator.hpp"
#include "defines.hpp"

int main(int argc, char* argv[])
{
    Renderer renderer("VineGen", 900, 900);
    if(!renderer.Init()) {
        std::cout << "[ERROR] Failed to initialize renderer" << std::endl;
        return EXIT_FAILURE;
    }

	VineGenerator vineGenerator(renderer);
	if(!vineGenerator.Init()) {
        std::cout << "[ERROR] Failed to initialize vine generator" << std::endl;
		return EXIT_FAILURE;
	}

    const double startTime = GetTimestamp();
    double lastFrameTime = startTime;
    while(!renderer.ShouldClose()) {
        const double currentFrameTime = GetTimestamp();
        const double dt = currentFrameTime - lastFrameTime;
        lastFrameTime = currentFrameTime;
        renderer.Update(dt);
    }

    std::cout << "Closed" << std::endl;
    return EXIT_SUCCESS;
}