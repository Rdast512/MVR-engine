<#
.SYNOPSIS
Updates the source references under this folder.
.DESCRIPTION
Keeps the current branch and skips dirty or detached worktrees and branches
without an upstream.
Known NVIDIA source archives are replaced in place with source-only Git
clones. These fetch source and text documentation through partial clone and
sparse checkout. Git LFS downloads and submodule checkouts are disabled.
.EXAMPLE
pwsh -File reference/update-repos.ps1
.EXAMPLE
pwsh -File reference/update-repos.ps1 -WhatIf
#>
[CmdletBinding(SupportsShouldProcess)]
param()

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$git = (Get-Command git -CommandType Application | Select-Object -First 1).Source
$referenceRoot = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$gitWithoutLfs = @('-c', 'filter.lfs.process=', '-c', 'filter.lfs.smudge=', '-c', 'filter.lfs.required=false')

$sourceSparsePaths = @(
    '*.c', '*.cc', '*.cpp', '*.cxx', '*.h', '*.hh', '*.hpp', '*.hxx', '*.inl', '*.inc', '*.ipp', '*.ixx'
    '*.cu', '*.cuh', '*.rc', '*.slang', '*.slangh', '*.glsl', '*.hlsl', '*.hlsli'
    '*.vert', '*.frag', '*.comp', '*.mesh', '*.task', '*.geom', '*.tesc', '*.tese'
    '*.rgen', '*.rmiss', '*.rchit', '*.rahit', '*.rint', '*.rcall'
    '*.cmake', '*.in', '*.cfg', '*.json', '*.yaml', '*.yml', '*.toml', '*.props', '*.msc'
    '*.ps1', '*.py', '*.sh', '*.bat', '*.cmd', '*.js', '*.ts', '*.css', '*.html', '*.md', '*.txt', '*.rst'
    'Makefile', 'README', 'LICENSE', 'COPYING', 'NOTICE', 'CONTRIBUTING', '*_LICENSE'
    '.gitignore', '.gitattributes', '.gitmodules', '.clang-format', '.clang-tidy', '.editorconfig'
    '!**/[Aa]ssets/**', '!**/[Mm]odels/**', '!**/[Tt]extures/**', '!**/[Mm]edia/**'
)

$snapshotSources = @{
    'RTXMG-main'                     = 'NVIDIA-RTX/RTXMG'
    'vk_animated_clusters'          = 'nvpro-samples/vk_animated_clusters'
    'vk_compute_mipmaps'             = 'nvpro-samples/vk_compute_mipmaps'
    'vk_denoise_dlssrr'              = 'nvpro-samples/vk_denoise_dlssrr'
    'vk_device_generated_cmds-master' = 'nvpro-samples/vk_device_generated_cmds'
    'vk_gltf_renderer'              = 'nvpro-samples/vk_gltf_renderer'
    'vk_lod_clusters'               = 'nvpro-samples/vk_lod_clusters'
    'vk_memory_decompression'       = 'nvpro-samples/vk_memory_decompression'
    'vk_mini_samples'               = 'nvpro-samples/vk_mini_samples'
    'vk_minimal_latest'             = 'nvpro-samples/vk_minimal_latest'
    'vk_partitioned_tlas-main'       = 'nvpro-samples/vk_partitioned_tlas'
    'vk_tessellated_clusters-main'   = 'nvpro-samples/vk_tessellated_clusters'
    'vk_timeline_semaphore'          = 'nvpro-samples/vk_timeline_semaphore'
}

function Get-ReferenceFolder([string] $Directory) {
    foreach ($folder in Get-ChildItem -LiteralPath $Directory -Directory -Force) {
        if (($folder.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
            $folder.Name -eq '.git' -or
            $folder.Name -like '.reference-*') {
            continue
        }

        $isSnapshot = $Directory -eq $referenceRoot -and $snapshotSources.ContainsKey($folder.Name)
        if ($isSnapshot -or (Test-Path -LiteralPath (Join-Path $folder.FullName '.git'))) {
            $folder
        } else {
            Get-ReferenceFolder $folder.FullName
        }
    }
}

function Convert-ReferenceSnapshot([IO.DirectoryInfo] $Folder) {
    $suffix = [guid]::NewGuid().ToString('N')
    $staging = Join-Path $referenceRoot ".reference-clone-$suffix"
    $previous = Join-Path $referenceRoot ".reference-replaced-$suffix"
    $url = 'https://github.com/' + $snapshotSources[$Folder.Name] + '.git'

    # Check all paths before moving directory trees.
    foreach ($path in @($Folder.FullName, $staging, $previous)) {
        if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($path)) -ne $referenceRoot) {
            throw "Snapshot path is outside the reference folder: $path"
        }
    }
    if ((Test-Path -LiteralPath $staging) -or (Test-Path -LiteralPath $previous)) {
        throw 'Temporary replacement path already exists.'
    }

    & $git clone --filter=blob:none --no-checkout --no-local --no-recurse-submodules -- $url $staging
    if ($LASTEXITCODE -ne 0) {
        throw "Clone failed. The original snapshot is still at $($Folder.FullName). Check $staging for a partial clone."
    }

    # Set sparse rules before the first checkout can request any file contents.
    $sourceSparsePaths | & $git @gitWithoutLfs -C $staging sparse-checkout set --no-cone --stdin
    if ($LASTEXITCODE -ne 0) {
        throw "Could not set source-only checkout rules in $staging."
    }
    & $git @gitWithoutLfs -C $staging read-tree -m -u --no-recurse-submodules HEAD
    if ($LASTEXITCODE -ne 0) {
        throw "Source checkout failed in $staging. The original snapshot is still at $($Folder.FullName)."
    }

    $original = Get-Item -LiteralPath $Folder.FullName -Force
    if (($original.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        (Test-Path -LiteralPath (Join-Path $Folder.FullName '.git'))) {
        throw "Snapshot changed during the clone. The new checkout is at $staging."
    }

    # Directory.Move refuses an existing destination rather than nesting into it.
    [IO.Directory]::Move($Folder.FullName, $previous)
    try {
        [IO.Directory]::Move($staging, $Folder.FullName)
    } catch {
        if (-not (Test-Path -LiteralPath $Folder.FullName)) {
            [IO.Directory]::Move($previous, $Folder.FullName)
        }
        throw
    }
    Remove-Item -LiteralPath $previous -Recurse -Force
    Write-Host "Replaced $($Folder.Name) with its upstream source checkout."
}

$folders = @(Get-ReferenceFolder $referenceRoot | Sort-Object FullName)
$failed = 0
foreach ($folder in $folders) {
    $path = $folder.FullName
    try {
        if (-not (Test-Path -LiteralPath (Join-Path $path '.git'))) {
            if ($PSCmdlet.ShouldProcess($path, 'Replace source archive with an upstream source-only checkout')) {
                Convert-ReferenceSnapshot $folder
            }
            continue
        }

        $topLevel = & $git -C $path rev-parse --show-toplevel
        if ($LASTEXITCODE -ne 0 -or [IO.Path]::GetFullPath($topLevel) -ne $path) {
            throw 'Git could not verify this folder as the worktree root.'
        }
        $branch = & $git -C $path symbolic-ref --quiet --short HEAD
        if ($LASTEXITCODE -ne 0) {
            Write-Host "Skip $($folder.Name) (detached HEAD)"
            continue
        }
        $upstream = & $git -C $path rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>$null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "Skip $($folder.Name) (branch $branch has no upstream)"
            continue
        }
        $changes = & $git --no-optional-locks -C $path status --porcelain=v1 --untracked-files=normal --ignore-submodules=none
        if ($LASTEXITCODE -ne 0) {
            throw 'Could not read worktree status.'
        }
        if ($changes) {
            Write-Host "Skip $($folder.Name) (local changes)"
            continue
        }

        if ($PSCmdlet.ShouldProcess($path, "Fast-forward $branch from $upstream")) {
            Write-Host "Updating $($folder.Name): $branch <- $upstream"
            # The fast-forward diffstat can otherwise fetch excluded blobs just to report their sizes.
            & $git @gitWithoutLfs -C $path pull --ff-only --no-rebase --no-stat --no-recurse-submodules
            if ($LASTEXITCODE -ne 0) {
                throw 'Fast-forward pull failed.'
            }
        }
    } catch {
        Write-Warning "$($folder.Name): $($_.Exception.Message)"
        $failed++
    }
}

if ($folders.Count -eq 0) {
    Write-Host 'No Git worktrees or known source snapshots found.'
}
if ($failed -gt 0) {
    Write-Warning "$failed update(s) failed. See the warnings above."
    exit 1
}
