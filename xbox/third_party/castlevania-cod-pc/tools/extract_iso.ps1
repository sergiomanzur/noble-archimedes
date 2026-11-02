# The files of an original Xbox disc image into a folder: build.bat runs this when assets holds only a disc image, as
# the build reads default.xbe and the game then plays from the files. The image is a whole disc dump or its game
# partition alone (xiso), read as the SDK's runtime/kernel/disc.cpp reads it.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\extract_iso.ps1 <image.iso> <folder>
param([Parameter(Mandatory = $true)] [string] $Iso, [Parameter(Mandatory = $true)] [string] $Out)
$ErrorActionPreference = 'Stop'
$sector = 2048
$file = [IO.File]::OpenRead((Resolve-Path -LiteralPath $Iso).Path)
$Out = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)  # .NET has its own current folder

function Read-At([long] $at, [int] $len) {
    $buf = New-Object byte[] $len
    $file.Position = $at
    $got = 0
    while ($got -lt $len) {
        $n = $file.Read($buf, $got, $len - $got)
        if ($n -le 0) { throw "$Iso ends early" }
        $got += $n
    }
    return , $buf
}

try {
    # the game partition: at the start of an xiso, else past the video partition of a whole disc (XGD1, XGD2); its
    # volume descriptor in sector 32: magic, root table sector and size, the magic again at +0x7EC
    $magic = 'MICROSOFT*XBOX*MEDIA'
    $base = -1
    foreach ($p in 0, 0x18300000, 0xFD90000) {
        if ($p + 33 * $sector -gt $file.Length) { continue }
        $v = Read-At ($p + 32 * $sector) $sector
        $a = [Text.Encoding]::ASCII.GetString($v, 0, $magic.Length)
        $b = [Text.Encoding]::ASCII.GetString($v, 0x7EC, $magic.Length)
        if ($a -eq $magic -and $b -eq $magic) {
            $base = [long] $p
            $root = [BitConverter]::ToUInt32($v, 0x14)
            $rootSize = [BitConverter]::ToUInt32($v, 0x18)
            break
        }
    }
    if ($base -lt 0) { throw "$Iso is no Xbox disc image" }

    $count = 0
    [long] $total = 0
    $copy = New-Object byte[] (8MB)
    $folders = New-Object System.Collections.Stack
    $folders.Push(@($root, $rootSize, $Out))
    while ($folders.Count) {
        $sec, $size, $dir = $folders.Pop()
        [IO.Directory]::CreateDirectory($dir) | Out-Null
        if ($size -eq 0) { continue }
        # a folder is a table of entries in a binary tree: +0 left and +2 right entry (in 4-byte units, 0 for none),
        # +4 sector, +8 size, +0xC attributes (0x10 folder), +0xD name length, +0xE name; 0xFFFF on the left is empty
        $table = Read-At ($base + [long] $sec * $sector) $size
        $todo = New-Object System.Collections.Stack
        $todo.Push(0)
        $seen = @{}
        while ($todo.Count) {
            $at = [int] $todo.Pop()
            if ($at + 0xE -gt $size -or $seen.ContainsKey($at)) { continue }
            $seen[$at] = $true
            $left = [BitConverter]::ToUInt16($table, $at)
            $right = [BitConverter]::ToUInt16($table, $at + 2)
            if ($left -eq 0xFFFF) { continue }
            $length = $table[$at + 0xD]
            if ($at + 0xE + $length -gt $size) { continue }
            if ($left) { $todo.Push([int] $left * 4) }
            if ($right) { $todo.Push([int] $right * 4) }
            $start = [BitConverter]::ToUInt32($table, $at + 4)
            $bytes = [BitConverter]::ToUInt32($table, $at + 8)
            $name = [Text.Encoding]::ASCII.GetString($table, $at + 0xE, $length)
            $path = [IO.Path]::Combine($dir, $name)
            if ($table[$at + 0xC] -band 0x10) {
                $folders.Push(@($start, $bytes, $path))
                continue
            }
            $to = [IO.File]::Create($path)
            try {
                $file.Position = $base + [long] $start * $sector
                [long] $remain = $bytes
                while ($remain -gt 0) {
                    $n = $file.Read($copy, 0, [int] [Math]::Min($copy.Length, $remain))
                    if ($n -le 0) { throw "$Iso ends early in $name" }
                    $to.Write($copy, 0, $n)
                    $remain -= $n
                }
            } finally { $to.Close() }
            $count++
            $total += $bytes
            if ($count % 200 -eq 0) { Write-Host ("      {0} files, {1} MB" -f $count, [long] ($total / 1MB)) }
        }
    }
    Write-Host ("      {0} files, {1} MB" -f $count, [long] ($total / 1MB))
} finally {
    $file.Close()
}
