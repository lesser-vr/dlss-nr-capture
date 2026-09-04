function Get-ArtifactHash([string]$LiteralPath) {
 $stream = [IO.File]::OpenRead($LiteralPath)
 $sha = [Security.Cryptography.SHA256]::Create()
 try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
 finally { $sha.Dispose(); $stream.Dispose() }
}
