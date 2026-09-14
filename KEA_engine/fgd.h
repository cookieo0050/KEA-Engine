#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <utility>

// ============================================================================
// fgd.h - FGD (Forge Game Data) parser
// ============================================================================
//
// Reads a TrenchBroom-compatible .fgd entity definition file so the engine
// knows which entity classes exist, what key/value properties each one
// carries, and which mesh file (if any) each one spawns at runtime.
//
// The .fgd file is an "editing aid": it also powers TrenchBroom's entity
// browser and 3D model preview, so we keep the parser aligned with the
// standard TrenchBroom syntax (@PointClass / @SolidClass / @BaseClass,
// base(...), size(...), model(...), "key" (type) : "default" : "help",
// choices blocks and spawnflags(flags) bitmasks).
// ============================================================================

enum class FgdClassType {
    Point,   // @PointClass  - a placed object, may carry a model
    Solid,   // @SolidClass  - a brush-based entity
    Base     // @BaseClass   - a shared property bundle to inherit via base(...)
};

struct FgdFlag {
    int bit = 0;              // bit mask value (1, 2, 4, ...)
    std::string name;         // display name
    bool defaultValue = false;// default enabled state
};

struct FgdKeyValue {
    std::string key;                                  // e.g. "model", "origin"
    std::string type;                                 // "string", "integer", "flags", ...
    std::string displayName;                          // in-editor name (first quoted field)
    std::string defaultValue;                         // may be empty
    std::string description;                          // may be empty
    bool isFlags = false;                             // spawnflags bitmask block
    bool isChoices = false;                           // choices enumeration block
    std::vector<FgdFlag> flags;                       // when isFlags
    std::vector<std::pair<int, std::string>> choices; // when isChoices
};

struct FgdClass {
    FgdClassType type = FgdClassType::Point;
    std::string name;                                  // classname, e.g. "func_model"
    std::string description;                           // quoted help after the class name
    std::vector<std::string> baseClasses;              // inherited base() classes
    std::vector<FgdKeyValue> keyValues;
    glm::vec3 sizeMin{ -8.0f, -8.0f, -8.0f };          // editor gizmo size
    glm::vec3 sizeMax{ 8.0f, 8.0f, 8.0f };
    std::string modelSpec;                             // hardcoded model("path") if set
    std::string modelPathKey;                          // keyvalue holding the mesh path
    std::string modelScaleKey;                         // keyvalue holding the model scale
};

struct FgdFile {
    bool loaded = false;
    std::string error;
    std::vector<FgdClass> classes;

    // Returns the class with the given classname, or nullptr.
    const FgdClass* find(const std::string& name) const;
};

class FgdParser {
public:
    // Loads and parses an .fgd file. On failure returns a FgdFile with
    // loaded=false and error set (classes parsed before the error are kept).
    static FgdFile load(const std::string& path);
};
