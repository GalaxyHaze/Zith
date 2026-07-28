import sys

def parse_wasm_imports(filename):
    with open(filename, 'rb') as f:
        data = f.read()
    
    # Check magic
    if data[0:4] != b'\x00asm':
        print("Not a WASM file")
        return
    
    idx = 8
    while idx < len(data):
        section_id = data[idx]
        idx += 1
        
        # Read LEB128 size
        size = 0
        shift = 0
        while True:
            byte = data[idx]
            idx += 1
            size |= (byte & 0x7f) << shift
            if (byte & 0x80) == 0:
                break
            shift += 7
            
        if section_id == 2: # Import section
            section_start = idx
            
            # Read LEB128 count
            count = 0
            shift = 0
            while True:
                byte = data[idx]
                idx += 1
                count |= (byte & 0x7f) << shift
                if (byte & 0x80) == 0:
                    break
                shift += 7
            
            print(f"Imports ({count}):")
            for i in range(count):
                # Read module name
                mod_len = 0
                shift = 0
                while True:
                    byte = data[idx]
                    idx += 1
                    mod_len |= (byte & 0x7f) << shift
                    if (byte & 0x80) == 0:
                        break
                    shift += 7
                mod = data[idx:idx+mod_len].decode('utf-8')
                idx += mod_len
                
                # Read field name
                field_len = 0
                shift = 0
                while True:
                    byte = data[idx]
                    idx += 1
                    field_len |= (byte & 0x7f) << shift
                    if (byte & 0x80) == 0:
                        break
                    shift += 7
                field = data[idx:idx+field_len].decode('utf-8')
                idx += field_len
                
                # Read kind
                kind = data[idx]
                idx += 1
                
                if kind == 0: # Function
                    # Read signature index
                    sig_idx = 0
                    shift = 0
                    while True:
                        byte = data[idx]
                        idx += 1
                        sig_idx |= (byte & 0x7f) << shift
                        if (byte & 0x80) == 0:
                            break
                        shift += 7
                    print(f"  {mod}.{field} (Function)")
                elif kind == 1: # Table
                    idx += 2
                    print(f"  {mod}.{field} (Table)")
                elif kind == 2: # Memory
                    idx += 2
                    print(f"  {mod}.{field} (Memory)")
                elif kind == 3: # Global
                    idx += 2
                    print(f"  {mod}.{field} (Global)")
            return
        idx += size

parse_wasm_imports(sys.argv[1])
