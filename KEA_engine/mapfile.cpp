// ============================================================================
// mapfile.cpp - Quake-style .map Level Loader
// ============================================================================
//
// WHAT THIS FILE IS
// ----------------------------------------------------------------------------
// Reads a text .map file (Quake/Source brush format - see Maps/Testroom.map)
// and produces a LevelData: render chunks (per-texture vertex lists), collision
// triangles, point lights and the player start position.
//
// HOW TO UNDERSTAND IT (the pipeline)
// ----------------------------------------------------------------------------
// 1. parseEntities(): walks the file's `{ }` nesting to find "entities".
//    depth 1 = entity ("info_player_start", "light", "worldspawn", ...).
//    depth 2 = brush (a solid block) made of planes.
//    A line like `"key" "value"` is a key/value property on an entity.
// 2. parsePlaneLine(): one brush face is defined by 3 points + a texture name +
//    UV axes + scale. The plane's normal and distance are derived from the points.
// 3. generateBrushGeometry(): the clever bit. A brush is the region inside N
//    half-spaces. This function intersects every triple of planes
//    (solve the 3x3 linear system) and keeps the point only if it lies inside
//    ALL planes. That gives the polygon (corner vertices) of every face. The
//    vertices are then sorted around the face centre so the polygon is convex
//    and can be triangulated correctly.
// 4. emitFace(): triangulates each face polygon (fan around vertex 0) and writes
//    8 floats per vertex into the render chunk for that texture:
//    [x y z | nx ny nz | u v]. The same positions are appended to
//    collisionVertices (3 floats each) for the physics/collision systems.
//
// KEY IDEAS
// ----------------------------------------------------------------------------
// - Coordinate conversion: Quake maps use "Z up, Y forward"; the engine uses
//   "Y up, Z forward". convertPos does the swap (x, y, z) -> (x, z, -y).
//   MAP_SCALE = 1/32 shrinks the huge map units into metres.
// - UVs: u = (dot(pos,uAxis)/scaleX + uOffset) / texWidth, same for v. Changing
//   the uAxis/scale in the map editor directly changes texture alignment.
// - "Brush" = a convex solid, "Entity" = an object that holds brushes/properties.
// ============================================================================
#include "mapfile.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
using namespace std;

struct Plane {
    glm::vec3 p0, p1, p2;
    glm::vec3 normal;
    float distance = 0.0f;
    string texture;
    glm::vec3 uAxis, vAxis;
    float uOffset = 0, vOffset = 0;
    float rotation = 0, scaleX = 1, scaleY = 1;
};

struct FaceGeom {
    string texture;
    glm::vec3 normal;
    vector<glm::vec3> polyVerts;
    int planeIndex = -1;
};

struct Brush {
    vector<Plane> planes;
    vector<FaceGeom> faces;
};

struct Entity {
    map<string, string> properties;
    vector<Brush> brushes;
};

static const float MAP_SCALE = 1.0f / 32.0f;

static glm::vec3 convertPos(const glm::vec3& p) {
    return glm::vec3(p.x, p.z, -p.y) * MAP_SCALE;
}
static glm::vec3 convertDir(const glm::vec3& n) {
    return glm::normalize(glm::vec3(n.x, n.z, -n.y));
}

static string trim(const string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Reads a texture-name token that may be quoted (e.g. "PhotoshopExtension_Image (2)")
// and strips the quotes. Plain unquoted names are returned unchanged.
static string readNameToken(istringstream& iss) {
    string token;
    if (!(iss >> token)) return "";
    if (token.empty() || token[0] != '"') return token;

    if (token.size() >= 2 && token.back() == '"') {
        return token.substr(1, token.size() - 2);
    }

    // Name contains spaces - read until the closing quote.
    string rest;
    if (getline(iss, rest, '"')) {
        token = token.substr(1) + rest;
    }
    else {
        token = token.substr(1);
    }
    return trim(token);
}

static Plane parsePlaneLine(const string& line) {
    istringstream iss(line);
    string tmp;
    Plane pl;

    iss >> tmp >> pl.p0.x >> pl.p0.y >> pl.p0.z >> tmp;
    iss >> tmp >> pl.p1.x >> pl.p1.y >> pl.p1.z >> tmp;
    iss >> tmp >> pl.p2.x >> pl.p2.y >> pl.p2.z >> tmp;
    pl.texture = readNameToken(iss);
    iss >> tmp >> pl.uAxis.x >> pl.uAxis.y >> pl.uAxis.z >> pl.uOffset >> tmp;
    iss >> tmp >> pl.vAxis.x >> pl.vAxis.y >> pl.vAxis.z >> pl.vOffset >> tmp;
    iss >> pl.rotation >> pl.scaleX >> pl.scaleY;

    pl.normal = glm::normalize(glm::cross(pl.p2 - pl.p0, pl.p1 - pl.p0));
    pl.distance = glm::dot(pl.normal, pl.p0);

    if (pl.scaleX == 0) pl.scaleX = 1;
    if (pl.scaleY == 0) pl.scaleY = 1;

    return pl;
}

static vector<Entity> parseEntities(const string& path) {
    ifstream file(path);
    vector<Entity> entities;
    if (!file.is_open()) {
        cerr << "MapLoader: could not open " << path << "\n";
        return entities;
    }

    int depth = 0;
    Entity currentEntity;
    Brush currentBrush;
    string rawLine;

    while (getline(file, rawLine)) {
        string line = trim(rawLine);
        if (line.empty() || line.rfind("//", 0) == 0) continue;

        if (line == "{") {
            depth++;
            if (depth == 1) currentEntity = Entity();
            else if (depth == 2) currentBrush = Brush();
            continue;
        }
        if (line == "}") {
            if (depth == 2) {
                currentEntity.brushes.push_back(currentBrush);
            }
            else if (depth == 1) {
                entities.push_back(currentEntity);
            }
            depth--;
            continue;
        }

        if (depth == 1) {
            size_t firstQuote = line.find('"');
            size_t secondQuote = line.find('"', firstQuote + 1);
            size_t thirdQuote = line.find('"', secondQuote + 1);
            size_t fourthQuote = line.find('"', thirdQuote + 1);
            if (firstQuote != string::npos && fourthQuote != string::npos) {
                string key = line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
                string value = line.substr(thirdQuote + 1, fourthQuote - thirdQuote - 1);
                currentEntity.properties[key] = value;
            }
        }
        else if (depth == 2) {
            currentBrush.planes.push_back(parsePlaneLine(line));
        }
    }

    return entities;
}

static void generateBrushGeometry(Brush& brush) {
    size_t n = brush.planes.size();
    vector<vector<glm::vec3>> faceVerts(n);
    const float EPS = 0.01f;

    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            for (size_t k = j + 1; k < n; k++) {
                glm::mat3 Mt(brush.planes[i].normal, brush.planes[j].normal, brush.planes[k].normal);
                glm::mat3 M = glm::transpose(Mt);
                float det = glm::determinant(M);
                if (fabs(det) < 1e-6f) continue;

                glm::vec3 d(brush.planes[i].distance, brush.planes[j].distance, brush.planes[k].distance);
                glm::vec3 p = glm::inverse(M) * d;

                bool valid = true;
                for (size_t m = 0; m < n; m++) {
                    if (glm::dot(brush.planes[m].normal, p) > brush.planes[m].distance + EPS) {
                        valid = false;
                        break;
                    }
                }
                if (!valid) continue;

                for (size_t face : { i, j, k }) {
                    auto& verts = faceVerts[face];
                    bool dup = false;
                    for (auto& v : verts) {
                        if (glm::distance(v, p) < 0.02f) { dup = true; break; }
                    }
                    if (!dup) verts.push_back(p);
                }
            }

    for (size_t i = 0; i < n; i++) {
        auto& verts = faceVerts[i];
        if (verts.size() < 3) continue;

        glm::vec3 centroid(0.0f);
        for (auto& v : verts) centroid += v;
        centroid /= (float)verts.size();

        glm::vec3 normal = brush.planes[i].normal;
        glm::vec3 u = glm::normalize(verts[0] - centroid);
        glm::vec3 v = glm::cross(normal, u);

        sort(verts.begin(), verts.end(), [&](const glm::vec3& a, const glm::vec3& b) {
            float angA = atan2(glm::dot(a - centroid, v), glm::dot(a - centroid, u));
            float angB = atan2(glm::dot(b - centroid, v), glm::dot(b - centroid, u));
            return angA < angB;
            });

        FaceGeom fg;
        fg.texture = brush.planes[i].texture;
        fg.normal = normal;
        fg.polyVerts = verts;
        fg.planeIndex = (int)i;
        brush.faces.push_back(fg);
    }
}

static void weldBrushVertices(vector<FaceGeom*>& faces, float epsilon = 0.01f) {
    for (auto* face : faces) {
        for (auto& v : face->polyVerts) {
            for (auto* otherFace : faces) {
                if (face == otherFace) continue;
                for (auto& ov : otherFace->polyVerts) {
                    if (glm::distance(v, ov) < epsilon) {
                        v = ov;
                        break;
                    }
                }
            }
        }
    }
}

static void fixBrushTJunctions(vector<FaceGeom*>& faces, float epsilon = 0.02f) {
    bool changed = true;
    int iterations = 0;
    while (changed && iterations < 10) {
        changed = false;
        iterations++;
        for (size_t i = 0; i < faces.size(); i++) {
            auto* face = faces[i];
            if (face->polyVerts.size() < 3) continue;
            for (size_t e = 0; e < face->polyVerts.size(); e++) {
                glm::vec3 v0 = face->polyVerts[e];
                glm::vec3 v1 = face->polyVerts[(e + 1) % face->polyVerts.size()];
                glm::vec3 edgeDir = v1 - v0;
                float edgeLen = glm::length(edgeDir);
                if (edgeLen < 1e-6f) continue;
                glm::vec3 edgeUnit = edgeDir / edgeLen;
                for (size_t j = 0; j < faces.size(); j++) {
                    if (i == j) continue;
                    auto* otherFace = faces[j];
                    for (auto& ov : otherFace->polyVerts) {
                        glm::vec3 toOv = ov - v0;
                        float proj = glm::dot(toOv, edgeUnit);
                        if (proj <= epsilon || proj >= edgeLen - epsilon) continue;
                        glm::vec3 closest = v0 + edgeUnit * proj;
                        if (glm::distance(ov, closest) > epsilon) continue;
                        face->polyVerts.insert(face->polyVerts.begin() + (e + 1), ov);
                        changed = true;
                        break;
                    }
                    if (changed) break;
                }
                if (changed) break;
            }
            if (changed) break;
        }
    }
}

static void seamPadBrushFaces(Brush& brush, float epsilon = 0.001f) {
    for (auto& face : brush.faces) {
        glm::vec3 n = brush.planes[face.planeIndex].normal;
        for (auto& v : face.polyVerts) {
            v += n * epsilon;
        }
    }
}

static void emitFace(const FaceGeom& face, const Plane& plane, glm::ivec2 texSize, LevelData& level) {
    if (face.polyVerts.size() < 3) return;
    float texW = (float)(texSize.x > 0 ? texSize.x : 128);
    float texH = (float)(texSize.y > 0 ? texSize.y : 128);

    auto& renderBuf = level.renderChunks[face.texture];

    auto emitVertex = [&](const glm::vec3& mapPos) {
        float u = (glm::dot(mapPos, plane.uAxis) / plane.scaleX + plane.uOffset) / texW;
        float v = (glm::dot(mapPos, plane.vAxis) / plane.scaleY + plane.vOffset) / texH;

        glm::vec3 enginePos = convertPos(mapPos);
        glm::vec3 engineNormal = convertDir(face.normal);

        renderBuf.push_back(enginePos.x); renderBuf.push_back(enginePos.y); renderBuf.push_back(enginePos.z);
        renderBuf.push_back(engineNormal.x); renderBuf.push_back(engineNormal.y); renderBuf.push_back(engineNormal.z);
        renderBuf.push_back(u); renderBuf.push_back(v);

        level.collisionVertices.push_back(enginePos.x);
        level.collisionVertices.push_back(enginePos.y);
        level.collisionVertices.push_back(enginePos.z);
        };

    for (size_t k = 1; k + 1 < face.polyVerts.size(); k++) {
        emitVertex(face.polyVerts[0]);
        emitVertex(face.polyVerts[k]);
        emitVertex(face.polyVerts[k + 1]);
    }
}

LevelData MapLoader::load(const string& path, const function<glm::ivec2(const string&)>& textureSizeLookup) {
    LevelData level;
    vector<Entity> entities = parseEntities(path);

    for (auto& entity : entities) {
        auto classIt = entity.properties.find("classname");
        if (classIt == entity.properties.end()) continue;

        MapEntity mapEntity;
        mapEntity.classname = classIt->second;
        mapEntity.properties = entity.properties;
        level.entities.push_back(mapEntity);

        if (classIt->second == "info_player_start") {
            auto originIt = entity.properties.find("origin");
            if (originIt != entity.properties.end()) {
                istringstream iss(originIt->second);
                glm::vec3 mapOrigin;
                iss >> mapOrigin.x >> mapOrigin.y >> mapOrigin.z;
                level.playerStart = convertPos(mapOrigin);
                level.hasPlayerStart = true;
            }
        }
        else if (classIt->second == "light") {
            PointLight light;
            light.color = glm::vec3(1.0f);
            light.intensity = 1.0f;
            light.radius = 6.0f;

            auto originIt = entity.properties.find("origin");
            if (originIt != entity.properties.end()) {
                istringstream iss(originIt->second);
                glm::vec3 mapOrigin;
                iss >> mapOrigin.x >> mapOrigin.y >> mapOrigin.z;
                light.position = convertPos(mapOrigin);
            }
            auto colorIt = entity.properties.find("color");
            if (colorIt != entity.properties.end()) {
                istringstream iss(colorIt->second);
                iss >> light.color.x >> light.color.y >> light.color.z;
            }
            auto intensityIt = entity.properties.find("intensity");
            if (intensityIt != entity.properties.end()) {
                light.intensity = stof(intensityIt->second);
            }
            auto radiusIt = entity.properties.find("radius");
            if (radiusIt != entity.properties.end()) {
                light.radius = stof(radiusIt->second) * MAP_SCALE;
            }

            level.pointLights.push_back(light);
        }

        for (auto& brush : entity.brushes) {
            generateBrushGeometry(brush);
        }

        vector<FaceGeom*> allFaces;
        for (auto& brush : entity.brushes) {
            for (auto& face : brush.faces) {
                allFaces.push_back(&face);
            }
        }
        weldBrushVertices(allFaces, 0.01f);
        fixBrushTJunctions(allFaces, 0.02f);

        for (auto& brush : entity.brushes) {
            seamPadBrushFaces(brush, 0.001f);
        }

        for (auto& brush : entity.brushes) {
            for (auto& face : brush.faces) {
                glm::ivec2 texSize = textureSizeLookup(face.texture);
                emitFace(face, brush.planes[face.planeIndex], texSize, level);
            }
        }
    }

    return level;
}

// ============================================================================
// Sidecar lighting config (.lighting.cfg)
// ----------------------------------------------------------------------------
// Shared by the editor (writes) and the runtime (reads). Simple key=value text
// file stored next to the .map. Load is read-only on the game side; a missing
// file simply leaves the defaults in place.
// ============================================================================

static vector<float> parseVec3(const string& s) {
    vector<float> v;
    stringstream ss(s);
    float f;
    while (ss >> f) v.push_back(f);
    return v;
}

string sidecarPathFor(const string& mapPath) {
    return mapPath.substr(0, mapPath.size() - 4) + ".lighting.cfg";
}

void saveLightingSidecar(const string& mapPath, const LightingState& ls) {
    ofstream out(sidecarPathFor(mapPath), ios::trunc);
    if (!out) return;
    out << "sunPitch = " << ls.sunPitch << "\n";
    out << "sunYaw = " << ls.sunYaw << "\n";
    out << "sunColor = " << ls.sunColor.r << " " << ls.sunColor.g << " " << ls.sunColor.b << "\n";
    out << "sunIntensity = " << ls.sunIntensity << "\n";
    out << "ambientColor = " << ls.ambientColor.r << " " << ls.ambientColor.g << " " << ls.ambientColor.b << "\n";
    out << "ambientIntensity = " << ls.ambientIntensity << "\n";
}

void loadLightingSidecar(const string& mapPath, LightingState& ls) {
    ifstream in(sidecarPathFor(mapPath));
    if (!in) return;  // no sidecar yet -> keep defaults
    string line;
    while (getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == string::npos) continue;
        string key = line.substr(0, eq);
        string val = line.substr(eq + 1);
        auto trimStr = [](string s) {
            size_t b = s.find_first_not_of(" \t\r\n");
            size_t e = s.find_last_not_of(" \t\r\n");
            if (b == string::npos) return string();
            return s.substr(b, e - b + 1);
        };
        key = trimStr(key);
        val = trimStr(val);
        if (key == "sunPitch") ls.sunPitch = (float)atof(val.c_str());
        else if (key == "sunYaw") ls.sunYaw = (float)atof(val.c_str());
        else if (key == "sunColor") { vector<float> v = parseVec3(val); if (v.size() >= 3) ls.sunColor = glm::vec3(v[0], v[1], v[2]); }
        else if (key == "sunIntensity") ls.sunIntensity = (float)atof(val.c_str());
        else if (key == "ambientColor") { vector<float> v = parseVec3(val); if (v.size() >= 3) ls.ambientColor = glm::vec3(v[0], v[1], v[2]); }
        else if (key == "ambientIntensity") ls.ambientIntensity = (float)atof(val.c_str());
    }
}