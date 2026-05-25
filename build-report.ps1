$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$reportDir = Join-Path $repoRoot "report"
$buildDir = Join-Path $reportDir "build"
$sourceTex = Join-Path $reportDir "main.tex"
$outputPdf = Join-Path $repoRoot "CO3037-report.pdf"
$fallbackPdf = Join-Path $repoRoot "CO3037-report.latest.pdf"

if (!(Test-Path -LiteralPath $sourceTex)) {
    throw "Cannot find report source: $sourceTex"
}

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

function Invoke-Checked {
    param(
        [string]$Command,
        [string[]]$Arguments,
        [string]$WorkingDirectory
    )

    Push-Location $WorkingDirectory
    try {
        & $Command @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "Command failed: $Command $($Arguments -join ' ')"
        }
    }
    finally {
        Pop-Location
    }
}

if ((Get-Command latexmk -ErrorAction SilentlyContinue) -and (Get-Command perl -ErrorAction SilentlyContinue)) {
    Invoke-Checked -Command "latexmk" -Arguments @(
        "-pdf",
        "-interaction=nonstopmode",
        "-halt-on-error",
        "-output-directory=build",
        "main.tex"
    ) -WorkingDirectory $reportDir
}
elseif (Get-Command pdflatex -ErrorAction SilentlyContinue) {
    $args = @(
        "-interaction=nonstopmode",
        "-halt-on-error",
        "-output-directory=build",
        "main.tex"
    )
    Invoke-Checked -Command "pdflatex" -Arguments $args -WorkingDirectory $reportDir
    Invoke-Checked -Command "pdflatex" -Arguments $args -WorkingDirectory $reportDir
}
elseif (Get-Command xelatex -ErrorAction SilentlyContinue) {
    $args = @(
        "-interaction=nonstopmode",
        "-halt-on-error",
        "-output-directory=build",
        "main.tex"
    )
    Invoke-Checked -Command "xelatex" -Arguments $args -WorkingDirectory $reportDir
    Invoke-Checked -Command "xelatex" -Arguments $args -WorkingDirectory $reportDir
}
else {
    throw "No LaTeX engine found. Please install latexmk, pdflatex, or xelatex."
}

$generatedPdf = Join-Path $buildDir "main.pdf"
if (!(Test-Path -LiteralPath $generatedPdf)) {
    throw "Build finished but PDF was not generated: $generatedPdf"
}

try {
    Copy-Item -LiteralPath $generatedPdf -Destination $outputPdf -Force
    Write-Host "PDF generated at $outputPdf"
}
catch {
    try {
        Copy-Item -LiteralPath $generatedPdf -Destination $fallbackPdf -Force
        Write-Warning "Could not overwrite $outputPdf because it is being used by another process."
        Write-Host "PDF generated at $generatedPdf"
        Write-Host "Fallback copy generated at $fallbackPdf"
    }
    catch {
        $timestampedPdf = Join-Path $repoRoot ("CO3037-report-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".pdf")
        Copy-Item -LiteralPath $generatedPdf -Destination $timestampedPdf -Force
        Write-Warning "Could not overwrite $outputPdf or $fallbackPdf because they are being used by another process."
        Write-Host "PDF generated at $generatedPdf"
        Write-Host "Timestamped copy generated at $timestampedPdf"
    }
}
