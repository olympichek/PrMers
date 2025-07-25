// src/core/BackupManager.cpp
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
#include "core/BackupManager.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 300
#endif
#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif
#include <gmpxx.h>
#include <atomic>
#include <thread>
#include <chrono>
namespace core {

BackupManager::BackupManager(cl_command_queue queue,
                             unsigned interval,
                             size_t vectorSize,
                             const std::string& savePath,
                             unsigned exponent,
                             const std::string& mode,
                             const uint64_t b1,
                             const uint64_t b2)
  : queue_(queue)
  , backupInterval_(interval)
  , vectorSize_(vectorSize)
  , savePath_(savePath.empty() ? "." : savePath)
  , exponent_(exponent)
  , mode_(mode)
  , b1_(b1)
  , b2_(b2)
{
    std::filesystem::create_directories(savePath_);
    auto base = std::to_string(exponent_) + mode_;
    if(b1_>0){
        base = std::to_string(exponent_) + mode_ + std::to_string(b1_);
    }
    mersFilename_ = savePath_ + "/" + base + ".mers";
    loopFilename_ = savePath_ + "/" + base + ".loop";
    exponentFilename_ = savePath_ + "/" + base + ".exponent";
    if(b2_>0){
        base = std::to_string(exponent_) + mode_ + std::to_string(b1_) + "_" + (std::to_string(b2_));
        hqFilename_   = savePath_ + "/" + base + ".hq";
        qFilename_    = savePath_ + "/" + base + ".q";
        loop2Filename_= savePath_ + "/" + base + ".loop2";
    }
    
}

uint64_t BackupManager::loadStatePM1S2(cl_mem hqBuf,
                                       cl_mem qBuf,
                                       size_t bytes)
{
    uint64_t resume = 0;
    std::ifstream loopIn(loop2Filename_);
    if (loopIn >> resume && resume > 0) {
        std::cout << "Stage-2 resume at iteration " << resume << std::endl;
        std::vector<uint64_t> tmp(bytes / sizeof(uint64_t));

        std::ifstream hqIn(hqFilename_, std::ios::binary);
        if (hqIn) {
            hqIn.read(reinterpret_cast<char*>(tmp.data()), bytes);
            clEnqueueWriteBuffer(queue_, hqBuf, CL_TRUE, 0, bytes, tmp.data(), 0, nullptr, nullptr);
        }

        std::ifstream qIn(qFilename_, std::ios::binary);
        if (qIn) {
            qIn.read(reinterpret_cast<char*>(tmp.data()), bytes);
            clEnqueueWriteBuffer(queue_, qBuf, CL_TRUE, 0, bytes, tmp.data(), 0, nullptr, nullptr);
        }
        std::cout << "Stage-2 buffers restored" << std::endl;
    }
    return resume;
}

void BackupManager::saveStatePM1S2(cl_mem hqBuf,
                                   cl_mem qBuf,
                                   uint64_t idx,
                                   size_t bytes)
{
    std::vector<uint64_t> tmp(bytes / sizeof(uint64_t));

    clEnqueueReadBuffer(queue_, hqBuf, CL_TRUE, 0, bytes, tmp.data(), 0, nullptr, nullptr);
    std::ofstream hqOut(hqFilename_, std::ios::binary);
    if (hqOut) hqOut.write(reinterpret_cast<char*>(tmp.data()), bytes);

    clEnqueueReadBuffer(queue_, qBuf, CL_TRUE, 0, bytes, tmp.data(), 0, nullptr, nullptr);
    std::ofstream qOut(qFilename_, std::ios::binary);
    if (qOut) qOut.write(reinterpret_cast<char*>(tmp.data()), bytes);

    std::ofstream loopOut(loop2Filename_);
    if (loopOut) loopOut << (idx + 1);

    std::cout << "Stage-2 backup saved at iteration " << idx + 1 << std::endl;
}


uint64_t BackupManager::loadState(std::vector<uint64_t>& x) {
    uint64_t resume = 0;

    // 1) Debug : afficher le chemin absolu du .loop
    auto absLoop = std::filesystem::absolute(loopFilename_);
    std::cout << "Looking for loop file at " << absLoop << std::endl;

    // 2) Essayer de lire resume, puis valider resume > 0
    std::ifstream loopIn(loopFilename_);
    if (loopIn >> resume && resume > 0) {
        std::cout << "Resuming from iteration " << resume
                  << " based on " << absLoop << std::endl;

        // 3) Charger le vecteur binaire
        std::ifstream mersIn(mersFilename_, std::ios::binary);
        if (mersIn) {
            mersIn.read(reinterpret_cast<char*>(x.data()),
                        x.size() * sizeof(uint64_t));
            std::cout << "Loaded state from "
                      << std::filesystem::absolute(mersFilename_)
                      << std::endl;
        } else {
            std::cerr << "Warning: could not open mers file at "
                      << std::filesystem::absolute(mersFilename_)
                      << " — starting with uninitialized data\n";
        }
    }
    else {
        // 4) Pas de fichier valide → nouvelle session
        std::cout << "No valid loop file, initializing fresh state\n";
        resume = 0;
        x.assign(x.size(), 0ULL);
        x[0] = (mode_ == "prp") ? 3ULL : 4ULL;
    }

    return resume;
}


void BackupManager::saveState(cl_mem buffer, uint64_t iter, const mpz_class* E_ptr) {
    std::vector<uint64_t> x(vectorSize_);
    clEnqueueReadBuffer(queue_, buffer, CL_TRUE,
                        0, vectorSize_ * sizeof(uint64_t),
                        x.data(), 0, nullptr, nullptr);

    std::ofstream mersOut(mersFilename_, std::ios::binary);
    if (mersOut) {
        mersOut.write(reinterpret_cast<const char*>(x.data()),
                      vectorSize_ * sizeof(uint64_t));
        std::cout << "\nState saved to " << mersFilename_ << std::endl;
    } else {
        std::cerr << "Error saving state to " << mersFilename_ << std::endl;
    }

    std::ofstream loopOut(loopFilename_);
    if (loopOut) {
        loopOut << (iter + 1);
        std::cout << "Loop iteration saved to " << loopFilename_ << std::endl;
    } else {
        std::cerr << "Error saving loop state to " << loopFilename_ << std::endl;
    }

   if (mode_ == "pm1" && E_ptr != nullptr) {
        std::atomic<bool> done{false};
        std::thread spinner([&]{
            const char seq[] = {'|','/','-','\\'};
            int i = 0;
            while (!done) {
                std::cout << "\rSaving exponent to " << exponentFilename_ << " " 
                        << seq[i++ % 4] << std::flush;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });

        std::ofstream expOut(exponentFilename_);
        if (expOut) {
            expOut << *E_ptr;
            expOut.close();
            done = true;
            spinner.join();
            std::cout << "\rSaved exponent to " << exponentFilename_ << "    \n";
        } else {
            done = true;
            spinner.join();
            std::cerr << "Error saving exponent to " << exponentFilename_ << std::endl;
        }
    }

}


mpz_class BackupManager::loadExponent() const {
    mpz_class result{0};
    std::ifstream expIn(exponentFilename_);
    if (expIn) {
        expIn >> result;
        std::cout << "Loaded exponent value from " << exponentFilename_ << std::endl;
    } else {
        std::cout << "No exponent file found at " << exponentFilename_
                  << " — defaulting to 0" << std::endl;
    }
    return result;
}


void BackupManager::clearState() const {
    std::error_code ec;
    if (std::filesystem::exists(mersFilename_, ec)) {
        std::filesystem::remove(mersFilename_, ec);
        std::cout << "Removed backup file: " << mersFilename_ << std::endl;
    }
    if (std::filesystem::exists(loopFilename_, ec)) {
        std::filesystem::remove(loopFilename_, ec);
        std::cout << "Removed loop file: " << loopFilename_ << std::endl;
    }
    if (std::filesystem::exists(exponentFilename_, ec)) {
        std::filesystem::remove(exponentFilename_, ec);
        std::cout << "Removed exponent file: " << exponentFilename_ << std::endl;
    }
    if (std::filesystem::exists(hqFilename_, ec)) {
        std::filesystem::remove(hqFilename_, ec);
        std::cout << "Removed qFilename_ file: " << qFilename_ << std::endl;
    }
    if (std::filesystem::exists(qFilename_, ec)) {
        std::filesystem::remove(qFilename_, ec);
        std::cout << "Removed qFilename_ file: " << qFilename_ << std::endl;
    }
    if (std::filesystem::exists(loop2Filename_, ec)) {
        std::filesystem::remove(loop2Filename_, ec);
        std::cout << "Removed loop2Filename_ file: " << loop2Filename_ << std::endl;
    }
}

} // namespace core
