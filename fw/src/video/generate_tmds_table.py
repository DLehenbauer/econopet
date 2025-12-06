#!/usr/bin/env python3
"""
Generate TMDS lookup table for font encoding.

This script calculates DC-balanced pixel level pairs and generates a lookup table
that maps color intensity combinations and pixel patterns to pre-encoded DVI symbols.

The output is included by tmds_encode.c to provide fast TMDS symbol lookup during
font rendering.

Usage:
    ./generate_tmds_table.py 3 > tmds_table.h

Based on code from:
- tmds_encode_font_2bpp.S (table generation comments)
- libdvi/tmds_table_gen.py (TMDSEncode class)
"""

def popcount(x):
    """Count the number of 1 bits in x."""
    n = 0
    while x:
        n += 1
        x = x & (x - 1)
    return n

def byteimbalance(x):
    """Equivalent to N1(q) - N0(q) in the DVI spec."""
    return 2 * popcount(x) - 8

class TMDSEncode:
    """
    Direct translation of "Figure 3-5. T.M.D.S. Encode Algorithm" 
    on page 29 of DVI 1.0 spec.
    """
    ctrl_syms = {
        0b00: 0b1101010100,
        0b01: 0b0010101011,
        0b10: 0b0101010100,
        0b11: 0b1010101011
    }

    def __init__(self):
        self.imbalance = 0

    def encode(self, d, c, de):
        if not de:
            self.imbalance = 0
            return self.ctrl_syms[c]
        # Minimise transitions
        q_m = d & 0x1
        if popcount(d) > 4 or (popcount(d) == 4 and not d & 0x1):
            for i in range(7):
                q_m = q_m | (~(q_m >> i ^ d >> i + 1) & 0x1) << i + 1
        else:
            for i in range(7):
                q_m = q_m | ((q_m >> i ^ d >> i + 1) & 0x1) << i + 1
            q_m = q_m | 0x100
        # Correct DC balance
        inversion_mask = 0x2ff
        q_out = 0
        if self.imbalance == 0 or byteimbalance(q_m & 0xff) == 0:
            q_out = q_m ^ (0 if q_m & 0x100 else inversion_mask)
            if q_m & 0x100:
                self.imbalance += byteimbalance(q_m & 0xff)
            else:
                self.imbalance -= byteimbalance(q_m & 0xff)
        elif (self.imbalance > 0) == (byteimbalance(q_m & 0xff) > 0):
            q_out = q_m ^ inversion_mask
            self.imbalance += ((q_m & 0x100) >> 7) - byteimbalance(q_m & 0xff)
        else:
            q_out = q_m
            self.imbalance += byteimbalance(q_m & 0xff) - ((~q_m & 0x100) >> 7)
        return q_out

def find_consistent_imbalance_values():
    """
    Find all values 0-255 grouped by their imbalance after TMDS encoding from imbalance=0.
    
    Returns:
        Dict mapping imbalance -> list of values that produce that imbalance
    """
    enc = TMDSEncode()
    by_imbalance = {}
    
    for val in range(256):
        enc.imbalance = 0
        enc.encode(val, 0, True)
        imb = enc.imbalance
        if imb not in by_imbalance:
            by_imbalance[imb] = []
        by_imbalance[imb].append(val)
    
    return by_imbalance

def find_dc_balanced_pairs():
    """
    Find all (even, odd) pairs that produce zero DC imbalance.
    
    Returns:
        Set of (even, odd) tuples that are DC balanced
    """
    enc = TMDSEncode()
    balanced_pairs = set()
    
    for even_val in range(256):
        enc.imbalance = 0
        enc.encode(even_val, 0, True)
        even_imbalance = enc.imbalance
        
        for odd_val in range(256):
            enc.imbalance = even_imbalance  # Reset to after even
            enc.encode(odd_val, 0, True)
            if enc.imbalance == 0:
                balanced_pairs.add((even_val, odd_val))
    
    return balanced_pairs

def find_best_levels_for_imbalance(imbalance, num_levels, by_imbalance, all_pairs):
    """
    Find the best even/odd pairs for a given imbalance value.
    
    For each brightness level target, selects the closest even value with the
    specified imbalance, then finds the best compensating odd value.
    
    Returns:
        Tuple of (levels_even, levels_odd, max_error) where max_error is the
        worst-case error across all levels.
    """
    even_candidates = sorted(by_imbalance.get(imbalance, []))
    
    if len(even_candidates) < num_levels:
        return None, None, float('inf')
    
    # Target brightness values evenly spaced across 0-255
    targets = [(i * 255) / (num_levels - 1) if num_levels > 1 else 0 for i in range(num_levels)]
    
    levels_even = []
    levels_odd = []
    errors = []
    
    for target in targets:
        # Find closest even candidate
        best_even = min(even_candidates, key=lambda v: abs(v - target))
        even_error = abs(best_even - target)
        
        # For odd, try even-1 first (like original), then even+1, then search nearby
        candidates = [best_even - 1, best_even + 1]
        best_odd = None
        
        for candidate in candidates:
            if 0 <= candidate <= 255 and (best_even, candidate) in all_pairs:
                best_odd = candidate
                break
        
        # If neither works, search for closest valid pairing
        if best_odd is None:
            valid_odds = [odd for odd in range(256) if (best_even, odd) in all_pairs]
            if valid_odds:
                best_odd = min(valid_odds, key=lambda v: abs(v - target))
        
        if best_odd is None:
            return None, None, float('inf')
        
        odd_error = abs(best_odd - target)
        
        levels_even.append(best_even)
        levels_odd.append(best_odd)
        errors.append(max(even_error, odd_error))  # Take max of even and odd error for this level
    
    max_error = max(errors)  # Worst-case error across all levels
    return levels_even, levels_odd, max_error

def find_dc_balanced_levels(num_levels):
    """
    Find DC-balanced even/odd pixel value pairs for the given number of levels.
    
    Uses a minimax approach across multiple imbalance groups:
    1. For each possible imbalance value (0, ±2, ±4, ±6, ±8)
    2. Calculate the best even/odd pairs and their per-level errors
    3. Select the imbalance that minimizes the maximum error across all levels
    
    This ensures all levels use consistent encoding behavior while achieving
    the best possible brightness accuracy.
    
    Returns:
        Tuple of (levels_even, levels_odd) lists
    """
    # Find values grouped by imbalance
    by_imbalance = find_consistent_imbalance_values()
    
    # Print available imbalance groups
    print("// Values by imbalance after encoding from imbalance=0:")
    for imb in sorted(by_imbalance.keys()):
        vals = by_imbalance[imb]
        print(f"//   imbalance={imb:+d}: {len(vals)} values, range 0x{min(vals):02x}-0x{max(vals):02x}")
    print()
    
    # Get all DC-balanced pairs for finding odd values
    all_pairs = find_dc_balanced_pairs()
    
    # Try each imbalance and find the one with minimum max error
    best_result = None
    best_max_error = float('inf')
    best_imbalance = None
    
    # Try imbalances in order of preference (smaller absolute value first for balance)
    imbalances_to_try = sorted(by_imbalance.keys(), key=lambda x: (abs(x), -x))
    
    print("// Searching for best imbalance (minimax over level errors):")
    for imbalance in imbalances_to_try:
        levels_even, levels_odd, max_error = find_best_levels_for_imbalance(
            imbalance, num_levels, by_imbalance, all_pairs
        )
        
        if levels_even is not None:
            print(f"//   imbalance={imbalance:+d}: max_error={max_error:.1f}")
            
            if max_error < best_max_error:
                best_max_error = max_error
                best_result = (levels_even, levels_odd)
                best_imbalance = imbalance
    
    print()
    
    if best_result is None:
        raise RuntimeError(f"Could not find valid DC-balanced levels for {num_levels} levels")
    
    print(f"// Selected imbalance={best_imbalance:+d} with max_error={best_max_error:.1f}")
    print(f"// Candidate pool ({len(by_imbalance[best_imbalance])} values with this imbalance):")
    print(f"//   {', '.join(f'0x{v:02x}' for v in sorted(by_imbalance[best_imbalance]))}")
    print()
    
    return best_result

def verify_dc_balance(levels_even, levels_odd, targets, bits_per_pixel):
    """
    Verify that ALL combinations of even[i]/odd[j] produce zero DC imbalance.
    
    This exhaustively tests every possible pairing since pixels can have any
    foreground/background color combination at even/odd positions.
    
    Also computes and reports the transition with greatest brightness error.
    
    Args:
        levels_even: List of even pixel values
        levels_odd: List of odd pixel values  
        targets: List of target brightness values for each level
        bits_per_pixel: Number of bits per pixel (for display purposes)
    
    Returns True if all pairs are balanced, raises RuntimeError otherwise.
    """
    enc = TMDSEncode()
    num_levels = len(levels_even)
    
    # Print the selected levels first
    print(f"// {num_levels}-level DC-balanced pixel values for {bits_per_pixel}bpp encoding")
    print(f"// Each (even, odd) pair produces zero DC imbalance when TMDS encoded")
    print(f"//")
    print(f"// Selected even values: {', '.join(f'0x{v:02x}' for v in levels_even)}")
    print(f"// Selected odd values:  {', '.join(f'0x{v:02x}' for v in levels_odd)}")
    print(f"//")
    print("// Level analysis (average brightness):")
    for i, (even_val, odd_val) in enumerate(zip(levels_even, levels_odd)):
        avg = (even_val + odd_val) / 2
        print(f"//   Level {i}: even=0x{even_val:02x}, odd=0x{odd_val:02x}, avg={avg:.1f}")
    print()
    
    # Now verify DC balance
    print(f"// Verifying DC balance for all {num_levels}×{num_levels} = {num_levels**2} even/odd combinations:")
    all_balanced = True
    failures = []
    
    # Track max error transition
    max_error = 0
    max_error_info = None
    
    for i in range(num_levels):
        for j in range(num_levels):
            even_val = levels_even[i]
            odd_val = levels_odd[j]
            
            enc.imbalance = 0
            enc.encode(even_val, 0, True)
            imb_after_even = enc.imbalance
            enc.encode(odd_val, 0, True)
            final_imb = enc.imbalance
            
            if final_imb != 0:
                all_balanced = False
                failures.append((i, j, even_val, odd_val, imb_after_even, final_imb))
            
            # Calculate brightness error for this transition
            even_error = abs(even_val - targets[i])
            odd_error = abs(odd_val - targets[j])
            transition_error = max(even_error, odd_error)
            
            if transition_error > max_error:
                max_error = transition_error
                max_error_info = (i, j, even_val, odd_val, targets[i], targets[j], even_error, odd_error)
    
    if all_balanced:
        print(f"//   All {num_levels**2} combinations verified: DC balanced ✓")
    else:
        print(f"//   FAILED! {len(failures)} combinations have non-zero imbalance:")
        for i, j, even_val, odd_val, imb_after_even, final_imb in failures:
            print(f"//     even[{i}]=0x{even_val:02x} (imb={imb_after_even:+d}) + odd[{j}]=0x{odd_val:02x} -> final_imb={final_imb}")
    
    # Report max error transition
    if max_error_info:
        i, j, even_val, odd_val, target_i, target_j, even_err, odd_err = max_error_info
        print(f"//   Max brightness error: {max_error:.1f} at transition even[{i}]/odd[{j}]")
        print(f"//     even[{i}]=0x{even_val:02x} (target={target_i:.1f}, error={even_err:.1f})")
        print(f"//     odd[{j}]=0x{odd_val:02x} (target={target_j:.1f}, error={odd_err:.1f})")
    
    print()
    
    if not all_balanced:
        raise RuntimeError(f"DC balance verification failed! {len(failures)} combinations unbalanced.")
    
    return True

# Create encoder instance for table generation
enc = TMDSEncode()

def level(bg, fg, x, pix, levels_even, levels_odd):
    """
    Get the pixel level for a given background/foreground palette,
    pixel position x, and pixel run pattern.
    
    Note: In the nibble, the leftmost pixel (x=0) is in the MSB (bit 3),
    and the rightmost pixel (x=3) is in the LSB (bit 0).
    """
    index = fg if pix & (8 >> x) else bg
    return (levels_odd if x & 1 else levels_even)[index]

def generate_table(levels_even, levels_odd):
    """Generate the tmds_table as a C array."""
    num_levels = len(levels_even)
    total_entries = num_levels * num_levels * 16 * 2
    
    print(f"// TMDS lookup table: {num_levels} bg levels × {num_levels} fg levels × 16 pixel patterns × 2 words")
    print(f'const uint32_t __not_in_flash("tmds_table") tmds_table[{num_levels} * {num_levels} * 16 * 2] = {{')
    
    words = []
    for background in range(num_levels):
        for foreground in range(num_levels):
            for pixrun in range(16):
                sym = list(enc.encode(level(background, foreground, x, pixrun, levels_even, levels_odd), 0, 1) for x in range(4))
                assert enc.imbalance == 0, f"DC imbalance at bg={background}, fg={foreground}, pixrun={pixrun}"
                words.append(sym[1] << 10 | sym[0])
                words.append(sym[3] << 10 | sym[2])
    
    # Print 8 words per line for readability
    for i in range(0, len(words), 8):
        line_words = words[i:i+8]
        line = "    " + ", ".join(f"0x{w:05x}" for w in line_words)
        if i + 8 < len(words):
            line += ","
        print(line)
    
    print("};")

if __name__ == "__main__":
    import sys
    
    # Require user to specify bits per pixel
    if len(sys.argv) != 2:
        print("Usage: python3 generate_tmds_table.py <bits_per_pixel>")
        print("  bits_per_pixel: 2 for 4 levels, 3 for 8 levels, etc.")
        sys.exit(1)
    
    bits_per_pixel = int(sys.argv[1])
    num_levels = 1 << bits_per_pixel  # 2^bits_per_pixel
    
    print("#pragma once")
    print()
    print(f"// Calculating {num_levels}-level DC-balanced values...")
    print()
    
    # Calculate DC-balanced levels
    levels_even, levels_odd = find_dc_balanced_levels(num_levels)
    
    # Target brightness values evenly spaced across 0-255
    targets = [(i * 255) / (num_levels - 1) if num_levels > 1 else 0 for i in range(num_levels)]
    
    # Verify all pairs are DC balanced (also prints the selected levels)
    verify_dc_balance(levels_even, levels_odd, targets, bits_per_pixel)
    
    # Generate the lookup table
    generate_table(levels_even, levels_odd)
