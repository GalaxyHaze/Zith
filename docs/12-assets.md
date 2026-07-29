## 12. Assets

> **Implementation status:** Assets are **spec-only**. The `asset` import keyword parses but the
> compiler does not validate or embed external files. See [impl-status.md](impl-status.md).

Assets are external files — JSON, images, other data — that the compiler validates and makes available at compile time.

### 12.1 Configuration

Declare asset paths in `ZithProject.toml`:

```toml
[project]
name = "my_game"
version = "0.1.0"

[assets]
assets = ["assets/", "../someOtherFolder"]
```

The compiler verifies that every declared path exists and is readable.

### 12.2 Importing Assets

```zith
import assets/data.json as Data;
import assets/sprites/player.png as PlayerSprite;
```

> `as` is mandatory here, to avoid conflicts between files that share a name but differ in extension.

Imported assets are available as compile-time constants.

### 12.3 Processing Assets at Compile Time

Combine assets with `const fn` to process them before the program runs:

```zith
import assets/config.json as ConfigData;

const fn parseConfig(data: []char): Config {
    // parse JSON at compile time
    JSON.parse(data)!
}

const CONFIG = parseConfig(ConfigData);   // runs at compile time
```

### 12.4 Runtime Assets

Assets not declared in `[assets]` are loaded at runtime with standard file I/O:

```zith
let runtimeData = fs.read("runtime/save.json")!;
```

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
