// #include "main.cpp"
#include <vulkan/vulkan_core.h>





class FluidEngine {
public:
	FluidEngine();

	// create pipelines
	// bool Init(Renderer& renderer);
	bool Update(VkCommandBuffer cmd, float dt);

private:

};

// int main(int argc, char *argv[]) {
void test() {
    //create Pipeline
	// Renderer renderer("API", 600, 600);
	// renderer.Init(); // initialize vulkan

	// ComputePipeline fluidDiff
}