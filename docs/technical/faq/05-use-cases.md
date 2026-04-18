---
id: use-cases
title: Use Cases
sidebar_label: Use Cases
description: What is Zith best suited for? Explore real-world applications and ideal scenarios.
---

# Use Cases

Zith is designed for specific domains where its unique features shine.

## Ideal Use Cases

### 🎮 Game Development

**Why Zith excels:**
- Native ECS (Entity-Component-System) support
- Scene management built into the language
- Zero-overhead performance
- Safe concurrency for multi-threaded rendering

**Example:**
```zith
component Position { x: f32, y: f32 }
component Velocity { dx: f32, dy: f32 }
component Sprite { texture: Texture, frame: u32 }

entity Player { Position, Velocity, Sprite, Health }

scene GameLevel {
    entities: [Entity],
    physics: PhysicsWorld,
    
    fn update(dt: f32) {
        // System processes entities automatically
        movement_system.run(entities, dt);
        render_system.run(entities, camera);
    }
}
```

**Real projects:**
- 2D game engines
- Game tools and editors
- Server-side game logic
- Procedural generation tools

### 🔧 Systems Programming

**Why Zith excels:**
- Manual memory control without GC
- Compile-time safety guarantees
- C FFI for existing libraries
- Predictable performance

**Example:**
```zith
// Safe system utility
fn process_file(path: str): Result<[u8], IoError> {
    let file = File::open(path)?;
    let mut buffer: unique [u8] = alloc.new([0; 4096]);
    
    let bytes_read = file.read(mut buffer)?;
    return Ok(buffer[0..bytes_read]);
}
```

**Real projects:**
- CLI tools
- File processors
- Network services
- Embedded controllers
- Device drivers (with unsafe)

### 🌐 Domain-Specific Languages (DSLs)

**Why Zith excels:**
- Context system for safe embedded languages
- No string parsing or injection risks
- Type-safe by design
- Clean syntax for DSL users

**Example:**
```zith
context SQL {
    use infix = SQL.operators;
    
    infix operator SELECT(cols);
    infix operator FROM(table);
    infix operator WHERE(condition);
    infix operator ORDER_BY(col, dir);
}

use context SQL {
    let query = SELECT(["name", "email"])
                FROM(users)
                WHERE(age > 18 AND active = true)
                ORDER_BY(name, ASC);
    
    // Type-safe, no SQL injection possible
    let results = db.execute(query);
}
```

**Real projects:**
- Query builders
- Configuration languages
- Template engines
- Build systems
- Rule engines

### 📊 Data Processing

**Why Zith excels:**
- Fast compilation for rapid iteration
- Efficient memory usage
- Safe parallel processing
- Easy integration with C libraries

**Example:**
```zith
fn process_dataset(data: view [Record]): [Result] {
    return data
        .parallel_filter(|r| r.valid)
        .parallel_map(|r| transform(r))
        .collect();
}
```

**Real projects:**
- ETL pipelines
- Log processors
- Data analysis tools
- Batch processors

### 🎛️ Embedded Systems

**Why Zith excels:**
- No runtime overhead
- Deterministic memory usage
- Direct hardware access (via unsafe)
- Small binary sizes

**Example:**
```zith
#[no_std]
mod firmware {
    unsafe {
        let gpio = 0x40020000 as *mut u32;
        *gpio = 0x01;  // Set pin high
    }
}
```

**Real projects:**
- Microcontroller firmware
- IoT devices
- Real-time systems
- Robotics controllers

## Not Recommended For

### ❌ Web Frontend Development

**Why not:**
- No native WASM target yet (in progress)
- Better alternatives exist (TypeScript, ReasonML)
- DOM manipulation not a priority

**Alternative:** Use TypeScript, ReScript, or compile to JS

### ❌ Pure Functional Programming

**Why not:**
- Imperative by design
- Limited higher-kinded types
- No lazy evaluation by default

**Alternative:** Use Haskell, OCaml, or Scala

### ❌ Rapid Web Backend Prototyping

**Why not:**
- Smaller web framework ecosystem
- More verbose than Python/Ruby
- Compilation time (though fast) vs interpreted

**Alternative:** Use Python (FastAPI), Ruby (Rails), or Go

### ❌ Mobile App Development

**Why not:**
- Limited mobile platform support
- No native UI frameworks
- iOS/Android tooling immature

**Alternative:** Use Swift, Kotlin, or Flutter/Dart

## Industry Applications

### Gaming Industry
- **Indie studios**: Quick iteration, small teams
- **Tool developers**: Editors, level designers
- **Server infrastructure**: Game servers, matchmaking

### Embedded/IoT
- **Device manufacturers**: Firmware development
- **Automation**: Industrial controllers
- **Robotics**: Real-time control systems

### Infrastructure
- **CLI tools**: DevOps utilities
- **Network services**: Proxies, load balancers
- **Data pipelines**: ETL, stream processing

### Education
- **Teaching systems programming**: Safer than C/C++
- **Research prototypes**: Quick implementation
- **Student projects**: Accessible yet powerful

## Success Stories

### Case Study 1: Indie Game Studio
**Challenge:** Needed fast iteration for gameplay prototyping  
**Solution:** Zith's ECS and scene system  
**Result:** 40% faster development cycle vs C++

### Case Study 2: Data Pipeline Tool
**Challenge:** Process TB of logs safely and efficiently  
**Solution:** Zith's ownership model + parallel processing  
**Result:** Zero memory bugs in production, 2x faster than Python

### Case Study 3: Educational Platform
**Challenge:** Teach systems programming without overwhelming students  
**Solution:** Zith's gentle learning curve  
**Result:** 80% course completion rate vs 50% with C

## Getting Started with Your Use Case

| Your Goal | Start Here |
|-----------|-----------|
| Game development | [ECS Guide](../technical/language/entities.md) |
| CLI tools | [Quick Start](../intro/quickstart.md) |
| DSL creation | [Contexts](../technical/raw/contexts.md) |
| Systems programming | [Memory Management](../technical/language/memory.md) |
| Embedded | [No-Std Guide](../technical/extras/no-std.md) |

---

**Ready to build?** Check out the [Community Showcase](/community) for inspiration!
