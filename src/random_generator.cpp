#include "random_generator.hpp"

RandomGenerator::RandomGenerator(unsigned int seed) : engine(seed), distribution(0.0, 1.0) {
    // Initialize the random engine with the provided seed
}

void RandomGenerator::setSeed(unsigned int seed) {
    engine.seed(seed);
}