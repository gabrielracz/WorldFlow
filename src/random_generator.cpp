#include "random_generator.hpp"

RandomGenerator::RandomGenerator(unsigned int initseed) : engine(initseed), distribution(0.0, 1.0), seed(initseed) {
    // Initialize the random engine with the provided seed
}

void RandomGenerator::setSeed(unsigned int newseed) {
    this->seed = seed;
    engine.seed(seed);
}