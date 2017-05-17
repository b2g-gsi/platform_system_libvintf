/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <unordered_map>
#include <sstream>
#include <string>

#include <vintf/parse_string.h>
#include <vintf/parse_xml.h>

namespace android {
namespace vintf {

/**
 * Slurps the device manifest file and add build time flag to it.
 */
class AssembleVintf {
public:
    template<typename T>
    static bool getFlag(const std::string& key, T* value) {
        const char *envValue = getenv(key.c_str());
        if (envValue == NULL) {
            std::cerr << "Required " << key << " flag." << std::endl;
            return false;
        }

        if (!parse(envValue, value)) {
            std::cerr << "Cannot parse " << envValue << "." << std::endl;
            return false;
        }
        return true;
    }

    static std::string read(std::basic_istream<char>& is) {
        std::stringstream ss;
        ss << is.rdbuf();
        return ss.str();
    }

    static bool assemble(std::basic_istream<char>& inFile,
                         std::basic_ostream<char>& outFile,
                         std::ifstream& checkFile,
                         bool outputMatrix) {
        std::string error;
        std::string fileContent = read(inFile);

        HalManifest halManifest;
        if (gHalManifestConverter(&halManifest, fileContent)) {
            if (halManifest.mType == SchemaType::DEVICE) {
                if (!getFlag("BOARD_SEPOLICY_VERS", &halManifest.device.mSepolicyVersion)) {
                    return false;
                }
            }

            if (outputMatrix) {
                CompatibilityMatrix generatedMatrix = halManifest.generateCompatibleMatrix();
                if (!halManifest.checkCompatibility(generatedMatrix, &error)) {
                    std::cerr << "FATAL ERROR: cannot generate a compatible matrix: "
                              << error << std::endl;
                }
                outFile << "<!-- \n"
                           "    Autogenerated skeleton compatibility matrix. \n"
                           "    Use with caution. Modify it to suit your needs.\n"
                           "    All HALs are set to optional.\n"
                           "    Many entries other than HALs are zero-filled and\n"
                           "    require human attention. \n"
                           "-->\n"
                        << gCompatibilityMatrixConverter(generatedMatrix);
            } else {
                outFile << gHalManifestConverter(halManifest);
            }
            outFile.flush();

            if (checkFile.is_open()) {
                CompatibilityMatrix checkMatrix;
                if (!gCompatibilityMatrixConverter(&checkMatrix, read(checkFile))) {
                    std::cerr << "Cannot parse check file as a compatibility matrix: "
                              << gCompatibilityMatrixConverter.lastError()
                              << std::endl;
                    return false;
                }
                if (!halManifest.checkCompatibility(checkMatrix, &error)) {
                    std::cerr << "Not compatible: " << error << std::endl;
                    return false;
                }
            }

            return true;
        }

        CompatibilityMatrix matrix;
        if (gCompatibilityMatrixConverter(&matrix, fileContent)) {
            KernelSepolicyVersion kernelSepolicyVers;
            Version sepolicyVers;
            if (matrix.mType == SchemaType::FRAMEWORK) {
                if (!getFlag("BOARD_SEPOLICY_VERS", &sepolicyVers)) {
                    return false;
                }
                if (!getFlag("POLICYVERS", &kernelSepolicyVers)) {
                    return false;
                }
                matrix.framework.mSepolicy = Sepolicy(kernelSepolicyVers,
                        {{sepolicyVers.majorVer,sepolicyVers.minorVer}});
            }
            outFile << gCompatibilityMatrixConverter(matrix);
            outFile.flush();

            if (checkFile.is_open()) {
                HalManifest checkManifest;
                if (!gHalManifestConverter(&checkManifest, read(checkFile))) {
                    std::cerr << "Cannot parse check file as a HAL manifest: "
                              << gHalManifestConverter.lastError()
                              << std::endl;
                    return false;
                }
                if (!checkManifest.checkCompatibility(matrix, &error)) {
                    std::cerr << "Not compatible: " << error << std::endl;
                    return false;
                }
            }

            return true;
        }

        std::cerr << "Input file has unknown format." << std::endl
                  << "Error when attempting to convert to manifest: "
                  << gHalManifestConverter.lastError() << std::endl
                  << "Error when attempting to convert to compatibility matrix: "
                  << gCompatibilityMatrixConverter.lastError() << std::endl;
        return false;
    }
};

}  // namespace vintf
}  // namespace android

void help() {
    std::cerr <<
"assemble_vintf: Checks if a given manifest / matrix file is valid and \n"
"    fill in build-time flags into the given file.\n"
"assemble_vintf -h\n"
"               Display this help text.\n"
"assemble_vintf -i <input file> [-o <output file>] [-m] [-c [<check file>]]\n"
"               Fill in build-time flags into the given file.\n"
"    -i <input file>\n"
"               Input file. Format is automatically detected.\n"
"    -o <input file>\n"
"               Optional output file. If not specified, write to stdout.\n"
"    -m\n"
"               a compatible compatibility matrix is\n"
"               generated instead; for example, given a device manifest,\n"
"               a framework compatibility matrix is generated. This flag\n"
"               is ignored when input is a compatibility matrix.\n"
"    -c [<check file>]\n"
"               After writing the output file, check compatibility between\n"
"               output file and check file.\n"
"               If -c is set but the check file is not specified, a warning\n"
"               message is written to stderr. Return 0.\n"
"               If the check file is specified but is not compatible, an error\n"
"               message is written to stderr. Return 1.\n";
}

int main(int argc, char **argv) {
    std::ifstream inFile;
    std::ofstream outFile;
    std::ifstream checkFile;
    std::ostream* outFileRef = &std::cout;
    bool outputMatrix = false;
    std::string inFilePath;
    int res;
    while((res = getopt(argc, argv, "hi:o:mc:")) >= 0) {
        switch (res) {
            case 'i': {
                inFilePath = optarg;
                inFile.open(optarg);
                if (!inFile.is_open()) {
                    std::cerr << "Failed to open " << optarg << std::endl;
                    return 1;
                }
            } break;

            case 'o': {
                outFile.open(optarg);
                if (!outFile.is_open()) {
                    std::cerr << "Failed to open " << optarg << std::endl;
                    return 1;
                }
                outFileRef = &outFile;
            } break;

            case 'm': {
                outputMatrix = true;
            } break;

            case 'c': {
                if (strlen(optarg) != 0) {
                    checkFile.open(optarg);
                    if (!checkFile.is_open()) {
                        std::cerr << "Failed to open " << optarg << std::endl;
                        return 1;
                    }
                } else {
                    std::cerr << "WARNING: no compatibility check is done on "
                              << inFilePath << std::endl;
                }
            } break;

            case 'h':
            default: {
                help();
                return 1;
            } break;
        }
    }

    if (!inFile.is_open()) {
        std::cerr << "Missing input file" << std::endl;
        help();
        return 1;
    }

    bool success = ::android::vintf::AssembleVintf::assemble(
            inFile, *outFileRef, checkFile, outputMatrix);


    return success ? 0 : 1;
}

