// ============================================================================
// objmesh.cpp - Wavefront .obj loader implementation
// ============================================================================
//
// Parsing strategy:
//   - Line-based, whitespace-tolerant (leading/trailing/tabs stripped per line).
//   - `f` corners may be "v", "v/vt", "v//vn" or "v/vt/vn"; indices that are
//     negative are resolved relative to the end of the coords list, the way OBJ
//     specifies. Polygons with more than 3 corners are fan-triangulated.
//   - When the file supplies normals, identical (v,vt,vn) corners are
//     deduplicated and SHARED between faces (smooth shading). Without normals
//     each triangle gets a flat face normal and no vertices are shared.
//   - The first material that actually sees a face (via usemtl) wins the
//     diffuse slot; mtllibs are scanned for its map_Kd. This is intentionally
//     simple: a single-texture model is the common case for props. Models with
//     several materials fall back to the first one's texture.
// ============================================================================
#include "objmesh.h"
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <cstdlib>
#include <cstring>
#include <iostream>
using namespace std;

namespace {

string joinPath(const string& dir, const string& file) {
    if (dir.empty()) return file;
    char last = dir.back();
    if (last == '/' || last == '\\') return dir + file;
    return dir + "/" + file;
}

string dirOf(const string& path) {
    size_t sep = path.find_last_of("/\\");
    if (sep == string::npos) return "";
    return path.substr(0, sep);
}

string baseOf(const string& path) {
    size_t sep = path.find_last_of("/\\");
    if (sep == string::npos) return path;
    return path.substr(sep + 1);
}

bool fileExists(const string& path) {
    ifstream f(path.c_str());
    return f.good();
}

string trimStr(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Splits "v/vt/vn" into up to three integers. Missing components become -1.
void parseCorner(const string& tok, int& v, int& t, int& n) {
    v = t = n = -1;
    size_t s1 = tok.find('/');
    if (s1 == string::npos) { v = atoi(tok.c_str()); return; }
    string a = tok.substr(0, s1);
    size_t s2 = tok.find('/', s1 + 1);
    string b = (s2 == string::npos) ? tok.substr(s1 + 1) : tok.substr(s1 + 1, s2 - s1 - 1);
    string c = (s2 == string::npos) ? "" : tok.substr(s2 + 1);
    if (!a.empty()) v = atoi(a.c_str());
    if (!b.empty()) t = atoi(b.c_str());
    if (!c.empty()) n = atoi(c.c_str());
}

// 1-based positive indices -> 0-based; negative index is relative to the end.
int resolveIndex(int idx, int count) {
    if (idx > 0) return idx - 1;
    if (idx < 0) return count + idx;
    return -1;
}

// Scans a Wavefront .mtl and fills newmtl -> map_Kd path (Kd texture map).
void parseMtlFile(const string& path, unordered_map<string, string>& matDiffuse) {
    ifstream f(path.c_str());
    if (!f.is_open()) return;
    string cur, line;
    while (getline(f, line)) {
        line = trimStr(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.find("newmtl ") == 0) {
            cur = trimStr(line.substr(7));
        }
        else if (!cur.empty() && line.find("map_Kd ") == 0) {
            matDiffuse[cur] = trimStr(line.substr(7));
        }
    }
}

} // namespace

bool ObjMesh::loadFromObj(const std::string& path, const std::string& texturesFolder) {
    loaded = false;
    error.clear();
    vertexData.clear();
    diffusePath.clear();
    diffuseTexture = Texture();
    boundsMin = glm::vec3(1e30f);
    boundsMax = glm::vec3(-1e30f);

    ifstream file(path.c_str());
    if (!file.is_open()) {
        error = "could not open file: " + path;
        return false;
    }

    vector<glm::vec3> positions;
    vector<glm::vec2> texcoords;
    vector<glm::vec3> normals;

    // A face is three corners, each corner = (v, vt, vn) raw 0-based/negative
    // indices as written in the file (not yet resolved) or -1 when absent.
    struct Corner { int v = -1, t = -1, n = -1; };
    struct Face { Corner c[3]; };
    vector<Face> faces;

    vector<string> mtllibs;
    string curMtl;
    string firstMtl;
    unordered_map<string, bool> usedMats;

    string dir = dirOf(path);
    string line;
    while (getline(file, line)) {
        line = trimStr(line);
        if (line.empty() || line[0] == '#') continue;

        if (line.rfind("v ", 0) == 0) {
            glm::vec3 p;
            istringstream ss(line.substr(2));
            if (ss >> p.x >> p.y >> p.z) {
                positions.push_back(p);
                boundsMin = glm::min(boundsMin, p);
                boundsMax = glm::max(boundsMax, p);
            }
        }
        else if (line.rfind("vt ", 0) == 0) {
            glm::vec2 uv;
            istringstream ss(line.substr(3));
            if (ss >> uv.x >> uv.y) texcoords.push_back(uv);
        }
        else if (line.rfind("vn ", 0) == 0) {
            glm::vec3 n;
            istringstream ss(line.substr(3));
            if (ss >> n.x >> n.y >> n.z) normals.push_back(n);
        }
        else if (line.rfind("mtllib ", 0) == 0) {
            mtllibs.push_back(trimStr(line.substr(7)));
        }
        else if (line.rfind("usemtl ", 0) == 0) {
            curMtl = trimStr(line.substr(7));
        }
        else if (line.rfind("f ", 0) == 0) {
            istringstream ss(line.substr(2));
            vector<Corner> poly;
            string tok;
            while (ss >> tok) {
                Corner c;
                parseCorner(tok, c.v, c.t, c.n);
                poly.push_back(c);
            }
            if (poly.size() < 3) continue;

            if (firstMtl.empty() && !curMtl.empty()) firstMtl = curMtl;
            if (!curMtl.empty()) usedMats[curMtl] = true;

            for (size_t i = 1; i + 1 < poly.size(); ++i) {
                Face f;
                f.c[0] = poly[0];
                f.c[1] = poly[i];
                f.c[2] = poly[i + 1];
                faces.push_back(f);
            }
        }
    }

    if (positions.empty() || faces.empty()) {
        error = "no usable geometry in: " + path;
        return false;
    }

    const bool hasNormals = !normals.empty();

    // ------------------------------------------------------------------
    // Resolve indices -> interleaved tri-soup vertex stream
    // ------------------------------------------------------------------
    vertexData.reserve((size_t)faces.size() * 3 * 8);
    unordered_map<string, int> cornerDedup;   // "v:vt:vn" -> vertex index (smooth)
    int nextVertex = 0;

    for (size_t fi = 0; fi < faces.size(); ++fi) {
        const Face& face = faces[fi];

        // Flat normal when the file has none (face normals, per triangle).
        glm::vec3 flatN(0.0f, 1.0f, 0.0f);
        if (!hasNormals) {
            int v0 = resolveIndex(face.c[0].v, (int)positions.size());
            int v1 = resolveIndex(face.c[1].v, (int)positions.size());
            int v2 = resolveIndex(face.c[2].v, (int)positions.size());
            if (v0 >= 0 && v1 >= 0 && v2 >= 0) {
                glm::vec3 e1 = positions[v1] - positions[v0];
                glm::vec3 e2 = positions[v2] - positions[v0];
                flatN = glm::normalize(glm::cross(e1, e2));
            }
        }

        for (int k = 0; k < 3; ++k) {
            const Corner& c = face.c[k];
            int vi = resolveIndex(c.v, (int)positions.size());
            if (vi < 0 || vi >= (int)positions.size()) {
                error = "face references missing position index in: " + path;
                return false;
            }

            int ti = resolveIndex(c.t, (int)texcoords.size());
            int ni = resolveIndex(c.n, (int)normals.size());

            glm::vec3 p = positions[vi];
            glm::vec2 uv = (ti >= 0 && ti < (int)texcoords.size()) ? texcoords[ti] : glm::vec2(0.0f, 0.0f);
            glm::vec3 nrm;
            if (hasNormals) {
                if (ni < 0 || ni >= (int)normals.size()) {
                    error = "face references missing normal index in: " + path;
                    return false;
                }
                nrm = normals[ni];
            }
            else {
                nrm = flatN;
            }

            int target = -1;
            if (hasNormals) {
                string key = to_string(vi) + ":" + to_string(ti) + ":" + to_string(ni);
                auto it = cornerDedup.find(key);
                if (it != cornerDedup.end()) {
                    target = it->second;   // reuse an already-emitted shared corner
                }
                else {
                    target = nextVertex++;
                    cornerDedup[key] = target;
                }
            }
            else {
                target = nextVertex++;
            }

            // new corner -> append; deduplicated corner -> its data is already in
            // the stream (identical pos/normal/uv by the (v,vt,vn) definition).
            if (target == (int)(vertexData.size() / 8)) {
                vertexData.push_back(p.x); vertexData.push_back(p.y); vertexData.push_back(p.z);
                vertexData.push_back(nrm.x); vertexData.push_back(nrm.y); vertexData.push_back(nrm.z);
                vertexData.push_back(uv.x); vertexData.push_back(uv.y);
            }
        }
    }

    if (vertexData.empty()) {
        error = "no triangles emitted from: " + path;
        return false;
    }
    vertexCount = (int)(vertexData.size() / 8);
    vertexData.shrink_to_fit();

    // ------------------------------------------------------------------
    // Resolve the diffuse texture from the first material that saw a face
    // ------------------------------------------------------------------
    if (usedMats.size() > 1) {
        cerr << "ObjMesh: " << path << " uses " << usedMats.size()
            << " materials; only the first is textured (single-texture models only)\n";
    }
    if (!firstMtl.empty()) {
        unordered_map<string, string> matDiffuse;
        for (const string& ml : mtllibs) {
            parseMtlFile(joinPath(dir, trimStr(ml)), matDiffuse);
        }
        auto it = matDiffuse.find(firstMtl);
        if (it != matDiffuse.end() && !it->second.empty()) {
            string rel = it->second;
            string cand = joinPath(dir, rel);
            if (fileExists(cand)) {
                diffusePath = cand;
            }
            else {
                string fallback = joinPath(texturesFolder, baseOf(rel));
                if (fileExists(fallback)) diffusePath = fallback;
            }
            // If neither exists, leave diffusePath empty -> white fallback.
        }
    }

    loaded = true;
    return true;
}

bool ObjMesh::uploadToGPU() {
    if (vertexData.empty()) {
        error = "no vertex data to upload";
        return false;
    }

    if (VAO == 0) {
        glGenVertexArrays(1, &VAO);
        glGenBuffers(1, &VBO);
    }
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, vertexData.size() * sizeof(float), vertexData.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    vertexCount = (int)(vertexData.size() / 8);

    // 1x1 white texture so draw() always binds something valid.
    if (fallbackTexture == 0) {
        glGenTextures(1, &fallbackTexture);
        glBindTexture(GL_TEXTURE_2D, fallbackTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        unsigned char px[4] = { 255, 255, 255, 255 };
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }

    if (!diffusePath.empty()) diffuseTexture.load(diffusePath);

    loaded = true;
    return true;
}

void ObjMesh::draw() const {
    if (VAO == 0 || vertexCount == 0) return;
    if (diffuseTexture.valid()) diffuseTexture.bind(0);
    else {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, fallbackTexture);
    }
    glBindVertexArray(VAO);
    glDrawArrays(GL_TRIANGLES, 0, vertexCount);
}

std::vector<float> ObjMesh::collisionVertices() const {
    vector<float> out;
    out.reserve((vertexData.size() / 8) * 3);
    for (size_t i = 0; i + 7 < vertexData.size(); i += 8) {
        out.push_back(vertexData[i]);
        out.push_back(vertexData[i + 1]);
        out.push_back(vertexData[i + 2]);
    }
    return out;
}

void ObjMesh::freeGPU() {
    if (VAO) glDeleteVertexArrays(1, &VAO);
    if (VBO) glDeleteBuffers(1, &VBO);
    if (fallbackTexture) glDeleteTextures(1, &fallbackTexture);
    VAO = VBO = fallbackTexture = 0;
    vertexCount = 0;
    vertexData.clear();
    diffuseTexture = Texture();
}