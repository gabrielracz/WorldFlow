#ifndef RANDOM_GENERATOR_HPP_
#define RANDOM_GENERATOR_HPP_

#include <random>
#include <type_traits>

class RandomGenerator {
private:
    std::mt19937 engine;
    std::uniform_real_distribution<double> distribution;

public:
    // Constructor with optional seed
    explicit RandomGenerator(unsigned int seed = std::random_device{}());
    
    // Reset the generator with a new seed
    void setSeed(unsigned int seed);
    
    // Generate a random number in range [min, max] with specified type
    template<typename T>
    T rand(T min, T max) {
        static_assert(std::is_arithmetic<T>::value, "T must be a numeric type");
        
        if constexpr (std::is_integral<T>::value) {
            // For integral types, use uniform_int_distribution
            std::uniform_int_distribution<T> intDist(min, max);
            return intDist(engine);
        } else {
            // For floating point, use our uniform_real_distribution
            return static_cast<T>(distribution(engine) * (max - min) + min);
        }
    }
};

#endif