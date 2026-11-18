param(
    [Parameter(Mandatory = $true)][string]$CsvPath,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$rows = @(Import-Csv -LiteralPath $CsvPath)
if ($rows.Count -eq 0) {
    throw "No measurement rows found in $CsvPath"
}

$groups = @(
    $rows | Group-Object workers | ForEach-Object {
        $groupRows = @($_.Group)
        $throughput = 0.0
        $peakMemory = 0.0
        foreach ($row in $groupRows) {
            $elapsed = [double]$row.elapsed_ms
            if ($elapsed -le 0) {
                throw "Invalid elapsed time in $CsvPath"
            }
            $throughput += [double]$row.transactions * 1000.0 / $elapsed
            $peakMemory += [double]$row.peak_working_set_kb
        }
        [pscustomobject]@{
            Workers = [int]$_.Name
            Throughput = $throughput / $groupRows.Count
            PeakMemory = $peakMemory / $groupRows.Count
        }
    } | Sort-Object Workers
)

function Write-LineChart {
    param(
        [string]$Title,
        [string]$YAxis,
        [string]$ValueProperty,
        [string]$FileName
    )

    $width = 900
    $height = 520
    $left = 105.0
    $right = 850.0
    $top = 70.0
    $bottom = 425.0
    $values = @($groups | ForEach-Object { [double]$_.$ValueProperty })
    $maximum = ($values | Measure-Object -Maximum).Maximum
    if ($maximum -le 0) {
        $maximum = 1.0
    }
    $maximum *= 1.15

    $parts = [System.Collections.Generic.List[string]]::new()
    $parts.Add('<svg xmlns="http://www.w3.org/2000/svg" width="900" height="520" viewBox="0 0 900 520">')
    $parts.Add('<rect width="100%" height="100%" fill="white"/>')
    $parts.Add(('<text x="450" y="34" text-anchor="middle" font-family="Segoe UI" font-size="21" font-weight="600">{0}</text>' -f $Title))
    for ($tick = 0; $tick -le 4; $tick++) {
        $fraction = $tick / 4.0
        $y = $bottom - ($fraction * ($bottom - $top))
        $label = $maximum * $fraction
        $parts.Add(('<line x1="{0}" y1="{1}" x2="{2}" y2="{1}" stroke="#d8dee8"/>' -f $left, $y, $right))
        $parts.Add(('<text x="{0}" y="{1}" text-anchor="end" font-family="Segoe UI" font-size="12" fill="#334155">{2:N1}</text>' -f ($left - 10), ($y + 4), $label))
    }
    $parts.Add(('<line x1="{0}" y1="{1}" x2="{0}" y2="{2}" stroke="#475569"/>' -f $left, $top, $bottom))
    $parts.Add(('<line x1="{0}" y1="{1}" x2="{2}" y2="{1}" stroke="#475569"/>' -f $left, $bottom, $right))

    $points = [System.Collections.Generic.List[string]]::new()
    for ($i = 0; $i -lt $groups.Count; $i++) {
        $x = $left + ($i * ($right - $left) / [Math]::Max(1, $groups.Count - 1))
        $value = [double]$groups[$i].$ValueProperty
        $y = $bottom - (($value / $maximum) * ($bottom - $top))
        $points.Add(('{0},{1}' -f $x, $y))
        $parts.Add(('<circle cx="{0}" cy="{1}" r="5" fill="#2563eb"/>' -f $x, $y))
        $parts.Add(('<text x="{0}" y="{1}" text-anchor="middle" font-family="Segoe UI" font-size="13" fill="#334155">{2}</text>' -f $x, ($bottom + 24), $groups[$i].Workers))
        $parts.Add(('<text x="{0}" y="{1}" text-anchor="middle" font-family="Segoe UI" font-size="11" fill="#1e40af">{2:N1}</text>' -f $x, ($y - 10), $value))
    }
    if ($points.Count -gt 1) {
        $parts.Add(('<polyline points="{0}" fill="none" stroke="#2563eb" stroke-width="3"/>' -f ($points -join ' ')))
    }
    $parts.Add(('<text x="477" y="492" text-anchor="middle" font-family="Segoe UI" font-size="14">Worker threads</text>'))
    $parts.Add(('<text x="23" y="248" text-anchor="middle" font-family="Segoe UI" font-size="14" transform="rotate(-90 23 248)">{0}</text>' -f $YAxis))
    $parts.Add('</svg>')
    [System.IO.File]::WriteAllLines((Join-Path $OutputDirectory $FileName), $parts)
}

Write-LineChart -Title 'CrossIPC Windows transaction throughput' `
    -YAxis 'Transactions per second (mean)' -ValueProperty Throughput `
    -FileName 'throughput.svg'
Write-LineChart -Title 'CrossIPC Windows peak working set' `
    -YAxis 'Peak working set (KiB, mean)' -ValueProperty PeakMemory `
    -FileName 'peak-working-set.svg'
Write-Output "Graphs written to $OutputDirectory"
