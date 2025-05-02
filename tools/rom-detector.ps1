# Scan a directory for ROM files
param (
    [string] $Path = ".",
    [string] $Destination = "./roms"
)

if (-Not ([System.Management.Automation.PSTypeName]'Win32Api').Type) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public class Win32Api {
    [DllImport("ntdll.dll")]
    public static extern uint RtlComputeCrc32(uint dwInitial, byte[] pData, int iLen);
}
"@
}

# Define the ROM table from Appendix B
$romTable = @(
    @{ Model="CBM 30xx";    Type="Character (8 Scanlines)"; Function="PET Character (Basic 3/4) (F10) [2316-004]";     Part="901447-10";  Size="2KB";  Address="-";         IC="2316B-004"; Version="-";       CRC32="d8408674" },
    @{ Model="CBM 30xx";    Type="Character (8 Scanlines)"; Function="PET Character (Swedish) (F10)";                  Part="901447-14";  Size="2KB";  Address="-";         IC="2316B";     Version="-";       CRC32="48c77d29" },
    @{ Model="CBM 30xx";    Type="Diagnostic";              Function="COMMODORE PET ROM TESTER";                       Part="901447-18";  Size="2KB";  Address="9800-9FFF"; IC="2316B";     Version="-";       CRC32="081c7aad" },
    @{ Model="CBM 30xx";    Type="Basic";                   Function="PET Basic 3 $C0 (H1) [6316-007]";                Part="901447-20";  Size="2KB";  Address="C000-C7FF"; IC="2316B-007"; Version="3";       CRC32="6aab64a5" },
    @{ Model="CBM 30xx";    Type="Basic";                   Function="PET Basic 3 $C (D6) [2332-007]";                 Part="901465-01";  Size="4KB";  Address="C000-CFFF"; IC="2332-007";  Version="3";       CRC32="63a7fe4a" },
    @{ Model="CBM 30xx";    Type="Basic";                   Function="PET Basic 3 $C8 (H5)";                           Part="901447-21";  Size="2KB";  Address="C800-CFFF"; IC="2316B-008"; Version="3";       CRC32="a8f6ff4c" },
    @{ Model="CBM 30xx";    Type="Basic";                   Function="PET Basic 3 $D0 (H2)";                           Part="901447-22";  Size="2KB";  Address="D000-D7FF"; IC="2316B-009"; Version="3";       CRC32="97f7396a" },
    @{ Model="CBM 30xx";    Type="Basic";                   Function="PET Basic 3 $D (D7) [2332-008]";                 Part="901465-02";  Size="4KB";  Address="D000-DFFF"; IC="2332-008";  Version="3";       CRC32="ae4cb035" },
    @{ Model="CBM 30xx";    Type="Basic";                   Function="PET Basic 3 $D8 (H6)";                           Part="901447-23";  Size="2KB";  Address="D800-DFFF"; IC="2316B";     Version="3";       CRC32="4cf8724c" },
    @{ Model="CBM 30xx";    Type="Editor";                  Function="PET Edit 3 40 B NoCRTC (D8) [2316-024]";         Part="901474-01";  Size="2KB";  Address="E000-E7FF"; IC="2316B";     Version="3";       CRC32="05db957e" },
    @{ Model="CBM 30xx";    Type="Editor";                  Function="PET Edit 3 40 N NoCRTC (D8; H3) [2316-011]";     Part="901447-24";  Size="2KB";  Address="E000-E7FF"; IC="2316B-011"; Version="3";       CRC32="e459ab32" },
    @{ Model="CBM 30xx";    Type="Kernal";                  Function="PET Basic 3 $F0 (H4)";                           Part="901447-25";  Size="2KB";  Address="F000-F7FF"; IC="2316B-012"; Version="3";       CRC32="8745fc8a" },
    @{ Model="CBM 30xx";    Type="Kernal";                  Function="PET Kernal 3 $F (D9) [2332-009]";                Part="901465-03";  Size="4KB";  Address="F000-FFFF"; IC="2332-009";  Version="3";       CRC32="f02238e2" },
    @{ Model="CBM 30xx";    Type="Kernal";                  Function="PET Basic 3 $F8 (H7)";                           Part="901447-26";  Size="2KB";  Address="F800-FFFF"; IC="2316B-013"; Version="3";       CRC32="fd2c1f87" },
    @{ Model="CBM 40xx";    Type="Basic";                   Function="PET Basic 4 $B (D5; UD10)";                      Part="901465-19";  Size="4KB";  Address="B000-BFFF"; IC="2332";      Version="4";       CRC32="3a5f5721" },
    @{ Model="CBM 40xx";    Type="Basic";                   Function="PET Basic 4 $C (D6; UD9) [6332-059]";            Part="901465-20";  Size="4KB";  Address="C000-CFFF"; IC="2332-059";  Version="4";       CRC32="0fc17b9c" },
    @{ Model="CBM 40xx";    Type="Basic";                   Function="PET Basic 4 $D (D7; UD8) [6332-096]";            Part="901465-21";  Size="4KB";  Address="D000-DFFF"; IC="2332-096";  Version="4";       CRC32="36d91855" },
    @{ Model="CBM 40xx";    Type="Editor";                  Function="PET Edit 4 40 B NoCRTC (D8) [6316-035]";         Part="901474-02";  Size="2KB";  Address="E000-E7FF"; IC="2316B-035"; Version="4";       CRC32="75ff4af7" },
    @{ Model="CBM 40xx";    Type="Editor";                  Function="PET Edit 4 40 N CRTC 50Hz (UD7)";                Part="901498-01";  Size="2KB";  Address="E000-E7FF"; IC="2316B";     Version="4";       CRC32="3370e359" },
    @{ Model="CBM 40xx";    Type="Editor";                  Function="PET Edit 4 40 N CRTC 50Hz (UD7)";                Part="editor.bin"; Size="2KB";  Address="E000-E7FF"; IC="2716";      Version="4";       CRC32="eb9f6e75" },
    @{ Model="CBM 40xx";    Type="Editor";                  Function="PET Edit 4 40 N CRTC 60Hz (UD7)";                Part="901499-01";  Size="2KB";  Address="E000-E7FF"; IC="2316B";     Version="4";       CRC32="5f85bdf8" },
    @{ Model="CBM 40xx";    Type="Editor";                  Function="PET Edit 4 40 N NoCRTC (D8) [6316-034]";         Part="901447-29";  Size="2KB";  Address="E000-E7FF"; IC="2316B-034"; Version="4";       CRC32="e5714d4c" },
    @{ Model="CBM 40xx";    Type="Editor";                  Function="PET Edit 4 CRTC 60Hz (PET 9`")";                 Part="970150-07";  Size="2KB";  Address="E000-E7FF"; IC="2716";      Version="4";       CRC32="" },
    @{ Model="CBM 40xx";    Type="Kernal";                  Function="PET Kernal 4 $F (D9; UD6) [6332-075]";           Part="901465-22";  Size="4KB";  Address="F000-FFFF"; IC="2332-075";  Version="4";       CRC32="cc5298a1" },
    @{ Model="CBM 40xx";    Type="Basic";                   Function="PET Basic 4.1 $B (D5; UD10) [6332-120]";         Part="901465-23";  Size="4KB";  Address="B000-BFFF"; IC="2332-120";  Version="4.1";     CRC32="ae3deac0" },
    @{ Model="CBM 80xx";    Type="Software";                Function="COMTEXT (UD12)";                                 Part="324482-03";  Size="4KB";  Address="9000-9FFF"; IC="2532";      Version="-";       CRC32="cc01c40a" },
    @{ Model="CBM 80xx";    Type="-";                       Function="Diagnostics 80 Col";                             Part="901481-01";  Size="4KB";  Address="F000-FFFF"; IC="2332";      Version="-";       CRC32="24e4a616" },
    @{ Model="CBM 80xx";    Type="Expansion";               Function="PET High Speed Graphik Rev 1B";                  Part="324381-01B"; Size="4KB";  Address="A000-AEFF"; IC="2332";      Version="-";       CRC32="c8e3bff9" },
    @{ Model="CBM 80xx";    Type="Editor";                  Function="PET Edit 4 80 B CRTC 50Hz (UD7) [2316-059]";     Part="901474-04";  Size="2KB";  Address="E000-E7FF"; IC="2316B-059"; Version="4";       CRC32="c1ffca3a" },
    @{ Model="CBM 80xx";    Type="Editor";                  Function="PET Edit 4 80 B CRTC 60Hz (UD7) [2316-041]";     Part="901474-03";  Size="2KB";  Address="E000-E7FF"; IC="2316B-041"; Version="4";       CRC32="5674dd5e" },
    @{ Model="CBM 80xx";    Type="Editor";                  Function="PET Edit 4 80 CRTC 60Hz (UD7)";                  Part="901499-03";  Size="2KB";  Address="E000-E7FF"; IC="2316B";     Version="4";       CRC32="" },
    @{ Model="CBM 80xx CR"; Type="Basic";                   Function="PET Basic 4 & Kernal 4 (UE7) [26011B-632]";      Part="324746-01";  Size="16KB"; Address="B000-FFFF"; IC="23128-632"; Version="4";       CRC32="0x03a25bb4"},
    @{ Model="CBM 80xx CR"; Type="Editor";                  Function="PET Edit 4 80 8296 DIN 50Hz (UE8)";              Part="324243-01";  Size="4KB";  Address="E000-EFFF"; IC="2532";      Version="4";       CRC32="0x4000e833"},
    @{ Model="CBM 8296";    Type="Diagnostic";              Function="DIAG 8296 v1.3";                                 Part="324806-01";  Size="4KB";  Address="F000-FFFF"; IC="2332";      Version="1.3";     CRC32="c670e91c" },
    @{ Model="PET 2001";    Type="-";                       Function="-";                                              Part="901447-16";  Size="2KB";  Address="-";         IC="2316B";     Version="-";       CRC32="" },
    @{ Model="PET 2001";    Type="Character (8 Scanlines)"; Function="PET Character (Basic 1/2) (A2) [6540-010]";      Part="901439-08";  Size="2KB";  Address="-";         IC="6540-010";  Version="-";       CRC32="54f32f45" },
    @{ Model="PET 2001";    Type="Character (8 Scanlines)"; Function="PET Character (Basic 1/2) (F10; A2)";            Part="901447-08";  Size="2KB";  Address="-";         IC="2316B-08";  Version="-";       CRC32="54f32f45" },
    @{ Model="PET 2001";    Type="Character (8 Scanlines)"; Function="PET Character Japanese";                         Part="901447-12";  Size="2KB";  Address="-";         IC="2316B";     Version="-";       CRC32="2c9c8d89" },
    @{ Model="PET 2001";    Type="Diagnostic";              Function="Diagnostic Clip";                                Part="901447-30";  Size="2KB";  Address="9800-9FFF"; IC="2316B";     Version="-";       CRC32="73fe1901" },
    @{ Model="PET 2001";    Type="Basic";                   Function="PET Col80 $B";                                   Part="901465-15";  Size="4KB";  Address="B000-BFFF"; IC="2332";      Version="-";       CRC32="" },
    @{ Model="PET 2001";    Type="Basic";                   Function="PET Col80 $C";                                   Part="901465-16";  Size="4KB";  Address="C000-CFFF"; IC="2332";      Version="-";       CRC32="" },
    @{ Model="PET 2001";    Type="Basic";                   Function="PET Col80 $D";                                   Part="901465-17";  Size="4KB";  Address="D000-DFFF"; IC="2332";      Version="-";       CRC32="" },
    @{ Model="PET 2001";    Type="Kernal";                  Function="-";                                              Part="901465-04";  Size="4KB";  Address="F000-FFFF"; IC="2332";      Version="-";       CRC32="" },
    @{ Model="PET 2001";    Type="Kernal";                  Function="PET Col80 $F";                                   Part="901465-18";  Size="4KB";  Address="F000-FFFF"; IC="2332";      Version="-";       CRC32="" },
    @{ Model="PET 2001";    Type="Basic";                   Function="PET Basic 1 $C0 (H1)";                           Part="901447-01";  Size="2KB";  Address="C000-C7FF"; IC="2316B-01";  Version="1";       CRC32="a055e33a" },
    @{ Model="PET 2001";    Type="Basic";                   Function="PET Basic 1 $C0 (H1) [6540-011]";                Part="901439-01";  Size="2KB";  Address="C000-C7FF"; IC="6540-011";  Version="1";       CRC32="a055e33a" },
    @{ Model="PET 2001";    Type="Basic";                   Function="PET Basic 1/2 $C8 (H5)";                         Part="901447-02";  Size="2KB";  Address="C800-CFFF"; IC="2316B-02";  Version="1";       CRC32="69fd8a8f" },
    @{ Model="PET 2001";    Type="Basic";                   Function="PET Basic 1/2 $C8 (H5) [6540-012]";              Part="901439-05";  Size="2KB";  Address="C800-CFFF"; IC="6540-012";  Version="1";       CRC32="69fd8a8f" },
    @{ Model="PET 2001";    Type="Basic";                   Function="PET Basic 1/2 $D0 (H2)";                         Part="901447-03";  Size="2KB";  Address="D000-D7FF"; IC="2316B-03";  Version="1";       CRC32="d349f2d4" },
    @{ Model="PET 2001";    Type="Basic";                   Function="PET Basic 1/2 $D0 (H2) [6540-013]";              Part="901439-02";  Size="2KB";  Address="D000-D7FF"; IC="6540-013";  Version="1";       CRC32="d349f2d4" },
    @{ Model="PET 2001";    Type="Basic";                   Function="PET Basic 1/2 $D8 (H6)";                         Part="901447-04";  Size="2KB";  Address="D800-DFFF"; IC="2316B-04";  Version="1";       CRC32="850544eb" },
    @{ Model="PET 2001";    Type="Basic";                   Function="PET Basic 1/2 $D8 (H6) [6540-014]";              Part="901439-06";  Size="2KB";  Address="D800-DFFF"; IC="6540-014";  Version="1";       CRC32="850544eb" },
    @{ Model="PET 2001";    Type="Editor";                  Function="PET Basic 1/2 $E0 (H3)";                         Part="901447-05";  Size="2KB";  Address="E000-E7FF"; IC="2316B-05";  Version="1";       CRC32="9e1c5cea" },
    @{ Model="PET 2001";    Type="Editor";                  Function="PET Basic 1/2 $E0 (H3) [6540-015]";              Part="901439-03";  Size="2KB";  Address="E000-E7FF"; IC="6540-015";  Version="1";       CRC32="9e1c5cea" },
    @{ Model="PET 2001";    Type="Kernal";                  Function="PET Basic 1/2 $F0 (H4)";                         Part="901447-06";  Size="2KB";  Address="F000-F7FF"; IC="2316B-06";  Version="1";       CRC32="661a814a" },
    @{ Model="PET 2001";    Type="Kernal";                  Function="PET Basic 1/2 $F0 (H4) [6540-016]";              Part="901439-04";  Size="2KB";  Address="F000-F7FF"; IC="6540-016";  Version="1";       CRC32="661a814a" },
    @{ Model="PET 2001";    Type="Kernal";                  Function="PET Basic 1/2 $F8 (H7)";                         Part="901447-07";  Size="2KB";  Address="F800-FFFF"; IC="2316B-07";  Version="1";       CRC32="c4f47ad1" },
    @{ Model="PET 2001";    Type="Kernal";                  Function="PET Basic 1/2 $F8 (H7) [6540-018]";              Part="901439-07";  Size="2KB";  Address="F800-FFFF"; IC="6540-018";  Version="1";       CRC32="c4f47ad1" },
    @{ Model="PET 2001";    Type="Basic";                   Function="PET Basic 2 $C0 (H1)";                           Part="901447-09";  Size="2KB";  Address="C000-C7FF"; IC="2316B-09";  Version="2";       CRC32="03cf16d0" },
    @{ Model="PET 2001";    Type="Basic";                   Function="PET Basic 2 $C0 (H1) [6540-019]";                Part="901439-09";  Size="2KB";  Address="C000-C7FF"; IC="6540-019";  Version="2";       CRC32="03cf16d0" },
    @{ Model="PET 2001";    Type="Basic";                   Function="PET Basic 3 $C0 (H1) [6540-020]";                Part="901439-13";  Size="2KB";  Address="C000-C7FF"; IC="6540-020";  Version="3";       CRC32="6aab64a5" },
    @{ Model="PET 2001";    Type="Basic";                   Function="PET Basic 3 $C8 (H5) [6540-021]";                Part="901439-14";  Size="2KB";  Address="C800-CFFF"; IC="6540-021";  Version="3";       CRC32="a8f6ff4c" },
    @{ Model="PET 2001";    Type="Basic";                   Function="PET Basic 3 $D0 (H2) [6540-022]";                Part="901439-15";  Size="2KB";  Address="D000-D7FF"; IC="6540-022";  Version="3";       CRC32="97f7396a" },
    @{ Model="PET 2001";    Type="Basic";                   Function="PET Basic 3 $D8 (H6) [6540-023]";                Part="901439-16";  Size="2KB";  Address="D800-DFFF"; IC="6540-023";  Version="3";       CRC32="4cf8724c" },
    @{ Model="PET 2001";    Type="Editor";                  Function="PET Basic 3 $E0 (H3) [6540-024]";                Part="901439-17";  Size="2KB";  Address="E000-E7FF"; IC="6540-024";  Version="3";       CRC32="e459ab32" },
    @{ Model="PET 2001";    Type="Kernal";                  Function="PET Basic 3 $F0 (H4) [6540-025]";                Part="901439-18";  Size="2KB";  Address="F000-F7FF"; IC="6540-025";  Version="3";       CRC32="8745fc8a" },
    @{ Model="PET 2001";    Type="Kernal";                  Function="PET Basic 3 $F8 (H7) [6540-026]";                Part="901439-19";  Size="2KB";  Address="F800-FFFF"; IC="6540-026";  Version="3";       CRC32="fd2c1f87" }
)

function Get-CRC32 {
    param (
        [string] $filePath
    )

    # Read the file into a byte array
    $fileBytes = [System.IO.File]::ReadAllBytes($filePath)
    $crc32 = [Win32Api]::RtlComputeCrc32(0, $fileBytes, $fileBytes.Length)
    return "{0:x8}" -f $crc32
}

# Ensure the directory exists
if (-Not (Test-Path $Path)) {
    Write-Error "Directory '$Path' does not exist."
    exit 1
}

# Process each file in the directory
Get-ChildItem -Path $Path -File | ForEach-Object {
    $filePath = $_.FullName
    $crc32 = Get-CRC32 -filePath $filePath

    # Check if the CRC-32 matches any entry in the ROM table
    $matches = $romTable | Where-Object { $_.CRC32 -eq $crc32 }
    if ($matches) {
        Write-Host "File: $($_.Name)"
        Write-Host "CRC-32: $crc32"

        $matches | ForEach-Object {
            $match = $_
            $part = $match.Part
            $destinationFile = Join-Path -Path $Destination -ChildPath "$part.bin"

            # Copy the file to the 'roms/' directory
            Copy-Item -Path $filePath -Destination $destinationFile -Force

            Write-Host "Part: $part"
            Write-Host "Copied to: $destinationFile"
        }
        Write-Host "-----------------------------------"
    } else {
        Write-Warning "File: $($_.Name): No match found for CRC-32: $crc32"
    }
}
