/*
 * Mersenne OpenCL Primality Test Host Code
 *
 * This code is inspired by:
 *   - "mersenne.cpp" by Yves Gallot (Copyright 2020, Yves Gallot) based on
 *     Nick Craig-Wood's IOCCC 2012 entry (https://github.com/ncw/ioccc2012).
 *   - The Armprime project, explained at:
 *         https://www.craig-wood.com/nick/armprime/
 *     and available on GitHub at:
 *         https://github.com/ncw/
 *   - Yves Gallot (https://github.com/galloty), author of Genefer 
 *     (https://github.com/galloty/genefer22), who helped clarify the NTT and IDBWT concepts.
 *   - The GPUOwl project (https://github.com/preda/gpuowl), which performs Mersenne
 *     searches using FFT and double-precision arithmetic.
 * This code performs a Mersenne prime search using integer arithmetic and an IDBWT via an NTT,
 * executed on the GPU through OpenCL.
 *
 * Author: Cherubrock
 *
 * This code is released as free software. 
 */
#include "core/ProofSet.hpp"
#include "io/Sha3Hash.h"
#include "util/Crc32.hpp"
#include "util/Timer.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iomanip>

namespace core {

// Words
Words::Words() = default;

Words::Words(const std::vector<uint64_t>& v)
  : data_{v} {}

const std::vector<uint64_t>& Words::data() const noexcept {
    return data_;
}

std::vector<uint64_t>& Words::data() noexcept {
    return data_;
}

Words Words::fromUint64(const std::vector<uint64_t>& host, uint32_t exponent) {
    (void)exponent;
    return Words(host);
}

// ProofSet
ProofSet::ProofSet(uint32_t exponent, uint32_t proofLevel)
  : E{exponent}, power{proofLevel} {
  if(exponent%2!=0){
      assert(E & 1); // E is supposed to be prime
    
    // Create proof directory
    std::filesystem::create_directories(proofPath(E));

    // Calculate checkpoint points using binary tree structure
    std::vector<uint32_t> spans;
    for (uint32_t span = (E + 1) / 2; spans.size() < power; span = (span + 1) / 2) { 
      spans.push_back(span); 
    }

    points.push_back(0);
    for (uint32_t p = 0, span = (E + 1) / 2; p < power; ++p, span = (span + 1) / 2) {
      for (uint32_t i = 0, end = static_cast<uint32_t>(points.size()); i < end; ++i) {
        points.push_back(points[i] + span);
      }
    }

    assert(points.size() == (1u << power));
    assert(points.front() == 0);

    points.front() = E;
    std::sort(points.begin(), points.end());

    assert(points.size() == (1u << power));
    assert(points.back() == E);

    points.push_back(uint32_t(-1)); // guard element

    // Verify all points are valid
    for (uint32_t p : points) {
      assert(p > E || isInPoints(E, power, p));
    }
  }
}

bool ProofSet::shouldCheckpoint(uint32_t iter) const {
  return isInPoints(E, power, iter);
}

void ProofSet::save(uint32_t iter, const std::vector<uint32_t>& words) {
  if (!shouldCheckpoint(iter)) {
    return;
  }

  // Create the file path for this iteration
  auto filePath = proofPath(E) / std::to_string(iter);
  
  // Write the words data to file
  std::ofstream file(filePath, std::ios::binary);
  if (!file) {
    throw std::runtime_error("Cannot create proof checkpoint file: " + filePath.string());
  }
  
  // Write CRC32 first, then the data
  uint32_t crc = computeCRC32(words.data(), words.size() * sizeof(uint32_t));
  file.write(reinterpret_cast<const char*>(&crc), sizeof(crc));
  file.write(reinterpret_cast<const char*>(words.data()), 
             words.size() * sizeof(uint32_t));
  
  if (!file.good()) {
    throw std::runtime_error("Error writing proof checkpoint file: " + filePath.string());
  }
}

Words ProofSet::fromUint64(const std::vector<uint64_t>& host, uint32_t exponent) {
    return Words::fromUint64(host, exponent);
}

uint32_t ProofSet::bestPower(uint32_t E) {
  // Best proof powers assuming no disk space concern.
  // We increment power by 1 for each fourfold increase of the exponent.
  // The values below produce power=10 at wavefront, and power=11 at 100Mdigits:
  // power=10 from 60M to 240M, power=11 from 240M up.

  //assert(E > 0);
  // log2(x)/2 is log4(x)
  int32_t power = 10 + static_cast<int32_t>(std::floor(std::log2(E / 60e6) / 2));
  power = std::max(power, 2);
  power = std::min(power, 12);
  return static_cast<uint32_t>(power);
}

bool ProofSet::isInPoints(uint32_t E, uint32_t power, uint32_t k) {
  if (k == E) { return true; } // special-case E
  uint32_t start = 0;
  for (uint32_t p = 0, span = (E + 1) / 2; p < power; ++p, span = (span + 1) / 2) {
    assert(k >= start);
    if (k > start + span) {
      start += span;
    } else if (k == start + span) {
      return true;
    }
  }
  return false;
}

std::filesystem::path ProofSet::proofPath(uint32_t E) {
  return std::filesystem::path(std::to_string(E)) / "proof";
}

bool ProofSet::isValidTo(uint32_t limitK) const {
  // Check if we have all required checkpoint files up to limitK
  for (uint32_t point : points) {
    if (point > limitK) break;
    if (point < E && !fileExists(point)) {
      return false;
    }
  }
  return true;
}

bool ProofSet::fileExists(uint32_t k) const {
  auto filePath = proofPath(E) / std::to_string(k);
  return std::filesystem::exists(filePath);
}

std::vector<uint32_t> ProofSet::load(uint32_t iter) const {
  if (!shouldCheckpoint(iter)) {
    throw std::runtime_error("Attempt to load non-checkpoint iteration: " + std::to_string(iter));
  }

  auto filePath = proofPath(E) / std::to_string(iter);
  std::ifstream file(filePath, std::ios::binary);
  if (!file) {
    throw std::runtime_error("Cannot open proof checkpoint file: " + filePath.string());
  }

  // Read CRC32 first
  uint32_t crc;
  file.read(reinterpret_cast<char*>(&crc), sizeof(crc));
  if (!file.good()) {
    throw std::runtime_error("Error reading CRC32 from proof checkpoint file: " + filePath.string());
  }

  // Calculate expected file size in 32-bit words: (E + 31) / 32
  uint32_t expectedWords = (E + 31) / 32;
  
  // Read the 32-bit words data
  std::vector<uint32_t> words(expectedWords);
  file.read(reinterpret_cast<char*>(words.data()), expectedWords * sizeof(uint32_t));
  if (!file.good()) {
    throw std::runtime_error("Error reading data from proof checkpoint file: " + filePath.string());
  }

  // Verify CRC32
  uint32_t computedCrc = computeCRC32(words.data(), words.size() * sizeof(uint32_t));
  if (crc != computedCrc) {
    throw std::runtime_error("CRC32 mismatch in proof checkpoint file: " + filePath.string());
  }

  return words;
}

Proof ProofSet::computeProof() const {
  // Start timing proof generation
  util::Timer timer;

  std::vector<std::vector<uint32_t>> middles;
  std::vector<uint64_t> hashes;

  // Initial hash of the final residue B
  auto B = load(E);
  auto hash = Proof::hashWords(E, B);

  // Pre-allocate maximum needed buffer pool (power levels use 2^p buffers max)
  uint32_t maxBuffers = (1u << power);
  std::vector<mpz_class> bufferPool(maxBuffers);

  // Main computation loop
  for (uint32_t p = 0; p < power; ++p) {
    assert(p == hashes.size());
    
    uint32_t s = (1u << (power - p - 1)); // Step size for this level
    uint32_t levelBuffers = (1u << p); // Number of buffers needed for this level
    uint32_t bufIndex = 0;
    
    // Clear buffers that will be used for this level
    for (uint32_t i = 0; i < levelBuffers; ++i) {
      bufferPool[i] = 0;
    }
    
    // Load residues and apply binary tree algorithm
    for (uint32_t i = 0; i < levelBuffers; ++i) {
      // PRPLL's formula: load checkpoint at points[s * (i * 2 + 1) - 1]
      uint32_t checkpointIndex = s * (i * 2 + 1) - 1;
      
      if (checkpointIndex >= points.size()) {
        continue;
      }
      
      uint32_t iteration = points[checkpointIndex];
      
      if (iteration > E || !shouldCheckpoint(iteration)) {
        continue;
      }
      
      auto w = load(iteration);
      bufferPool[bufIndex] = convertToGMP(w);
      bufIndex++;
      
      // Apply hashes from previous levels
      for (uint32_t k = 0; i & (1u << k); ++k) {
        assert(k <= p - 1);
        if (bufIndex < 2) {
          std::cerr << "Error: need at least 2 buffers for expMul, have " << bufIndex << std::endl;
          continue;
        }
        
        bufIndex--;
        uint64_t h = hashes[p - 1 - k]; // Hash from previous level
        
        // PRPLL's expMul: (bufIndex-1) := (bufIndex-1)^h * bufIndex
        mpz_class temp = mersennePowMod(bufferPool[bufIndex - 1], h, E); // A^h mod (2^E - 1)
        mpz_class result = temp * bufferPool[bufIndex]; // A^h * B
        bufferPool[bufIndex - 1] = mersenneReduce(result, E); // Optimized Mersenne reduction
        
        // Clear the consumed buffer
        bufferPool[bufIndex] = 0;
      }
    }
    
    if (bufIndex != 1) {
      std::cerr << "Warning: expected bufIndex=1, got " << bufIndex << std::endl;
    }
    
    // Convert the final result to words format
    auto levelResult = convertFromGMP(bufferPool[0]);
    
    if (levelResult.empty()) {
      throw std::runtime_error("Read ZERO during proof generation at level " + std::to_string(p));
    }
    
    // Store the result as middle for this level
    middles.push_back(levelResult);
    
    // Update hash chain with this level's middle
    hash = Proof::hashWords(E, hash, levelResult);
    uint64_t newHash = hash[0]; // The first 64 bits of the hash
    hashes.push_back(newHash);
    
    // Show middle and hash for the current level
    uint64_t middleRes64 = Proof::res64(levelResult);
    std::cout << "proof [" << p << "] : M " << std::hex << std::setfill('0') << std::setw(16) << middleRes64 
              << ", h " << std::setw(16) << newHash << std::dec << std::endl;
  }
  
  // Display proof generation time
  double elapsed = timer.elapsed();
  std::cout << "Proof generated in " << std::fixed << std::setprecision(2) << elapsed << " seconds." << std::endl;
  
  return Proof{E, std::move(B), std::move(middles)};
}

mpz_class ProofSet::convertToGMP(const std::vector<uint32_t>& words) const {
  mpz_class result;
  // Use GMP's optimized mpz_import function
  mpz_import(result.get_mpz_t(), words.size(), -1 /*order: LSWord first*/, sizeof(uint32_t), 0 /*endian: native*/, 0 /*nails*/, words.data());
  return result;
}

// Optimized modular reduction for Mersenne numbers: x mod (2^E - 1)
// Uses the identity: X mod (2^E - 1) ≡ (Xlo + Xhi) mod (2^E - 1)
mpz_class ProofSet::mersenneReduce(const mpz_class& x, uint32_t E) const {
  // For small numbers, use regular mod
  if (mpz_sizeinbase(x.get_mpz_t(), 2) <= E + 1) {
    return x;
  }
  
  // Create Mersenne modulus: 2^E - 1
  mpz_class mersenne_mod = 1;
  mersenne_mod <<= E;
  mersenne_mod -= 1;
  
  // Split x into high and low parts
  // xlo = x & (2^E - 1)  (low E bits)
  mpz_class xlo = x & mersenne_mod;
  
  // xhi = x >> E  (remaining high bits)
  mpz_class xhi = x >> E;
  
  // Add high and low parts
  mpz_class result = xlo + xhi;
  
  // If result >= 2^E - 1, subtract the modulus
  if (result >= mersenne_mod) {
    result -= mersenne_mod;
  }
  
  return result;
}

// Optimized modular exponentiation for Mersenne numbers: base^exp mod (2^E - 1)
// Uses fast Mersenne reduction at each step instead of general division
mpz_class ProofSet::mersennePowMod(const mpz_class& base, uint64_t exp, uint32_t E) const {
  if (exp == 0) {
    return mpz_class(1);
  }
  
  if (exp == 1) {
    return mersenneReduce(base, E);
  }
  
  // Initialize result to 1
  mpz_class result = 1;
  
  // Copy base and reduce it
  mpz_class square = mersenneReduce(base, E);
  
  // Binary exponentiation with fast Mersenne reduction
  while (exp > 0) {
    if (exp & 1) {
      // result = result * square mod (2^E - 1)
      mpz_class temp = result * square;
      result = mersenneReduce(temp, E);
    }
    
    exp >>= 1;
    if (exp > 0) {
      // square = square * square mod (2^E - 1)
      mpz_class temp = square * square;
      square = mersenneReduce(temp, E);
    }
  }
  
  return result;
}

std::vector<uint32_t> ProofSet::convertFromGMP(const mpz_class& gmp_val) const {
  size_t wordCount = (E + 31) / 32;
  std::vector<uint32_t> data(wordCount, 0);
  
  // Use GMP's optimized mpz_export function
  size_t actualWords = 0;
  mpz_export(data.data(), &actualWords, -1 /*order: LSWord first*/, sizeof(uint32_t), 0 /*endian: native*/, 0 /*nails*/, gmp_val.get_mpz_t());
  
  // Note: actualWords may be less than wordCount if the number has leading zeros
  // The vector is already zero-initialized, so this is correct
  return data;
}

double ProofSet::diskUsageGB(uint32_t E, uint32_t power) {
  // Calculate disk usage in GB for proof files
  // Formula from PRPLL: ldexp(E, -33 + int(power)) * 1.05
  if (power == 0) return 0.0;
  return std::ldexp(static_cast<double>(E), -33 + static_cast<int>(power)) * 1.05;
}

} // namespace core
