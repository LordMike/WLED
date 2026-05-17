Import('env')
import subprocess
import json
import re
from datetime import datetime, timezone

def get_github_repo():
    """Extract GitHub repository name from git remote URL.
    
    Uses the remote that the current branch tracks, falling back to 'origin'.
    This handles cases where repositories have multiple remotes or where the
    main remote is not named 'origin'.
    
    Returns:
        str: Repository name in 'owner/repo' format for GitHub repos,
             'unknown' for non-GitHub repos, missing git CLI, or any errors.
    """
    try:
        remote_name = 'origin'  # Default fallback
        
        # Try to get the remote for the current branch
        try:
            # Get current branch name
            branch_result = subprocess.run(['git', 'rev-parse', '--abbrev-ref', 'HEAD'], 
                                         capture_output=True, text=True, check=True)
            current_branch = branch_result.stdout.strip()
            
            # Get the remote for the current branch
            remote_result = subprocess.run(['git', 'config', f'branch.{current_branch}.remote'], 
                                         capture_output=True, text=True, check=True)
            tracked_remote = remote_result.stdout.strip()
            
            # Use the tracked remote if we found one
            if tracked_remote:
                remote_name = tracked_remote
        except subprocess.CalledProcessError:
            # If branch config lookup fails, continue with 'origin' as fallback
            pass
        
        # Get the remote URL for the determined remote
        result = subprocess.run(['git', 'remote', 'get-url', remote_name], 
                              capture_output=True, text=True, check=True)
        remote_url = result.stdout.strip()
        
        # Check if it's a GitHub URL
        if 'github.com' not in remote_url.lower():
            return None
        
        # Parse GitHub URL patterns:
        # https://github.com/owner/repo.git
        # git@github.com:owner/repo.git
        # https://github.com/owner/repo
        
        # Remove .git suffix if present
        if remote_url.endswith('.git'):
            remote_url = remote_url[:-4]
        
        # Handle HTTPS URLs
        https_match = re.search(r'github\.com/([^/]+/[^/]+)', remote_url, re.IGNORECASE)
        if https_match:
            return https_match.group(1)
        
        # Handle SSH URLs
        ssh_match = re.search(r'github\.com:([^/]+/[^/]+)', remote_url, re.IGNORECASE)
        if ssh_match:
            return ssh_match.group(1)
        
        return None
        
    except FileNotFoundError:
        # Git CLI is not installed or not in PATH
        return None
    except subprocess.CalledProcessError:
        # Git command failed (e.g., not a git repo, no remote, etc.)
        return None
    except Exception:
        # Any other unexpected error
        return None

def get_git_commit_short():
    """Return a short git commit id, including '-dirty' for local changes."""
    try:
        result = subprocess.run(['git', 'rev-parse', '--short=8', 'HEAD'],
                              capture_output=True, text=True, check=True)
        commit = result.stdout.strip()
        dirty = subprocess.run(['git', 'diff', '--quiet', '--ignore-submodules'],
                             capture_output=True)
        staged_dirty = subprocess.run(['git', 'diff', '--cached', '--quiet', '--ignore-submodules'],
                                    capture_output=True)
        if dirty.returncode != 0 or staged_dirty.returncode != 0:
            commit += "-dirty"
        return commit
    except Exception:
        return "unknown"

def get_build_date():
    """Return compact UTC build date for human-facing build labels."""
    return datetime.now(timezone.utc).strftime("%Y%m%d")

# WLED version is managed by package.json; this is picked up in several places
# - It's integrated in to the UI code
# - Here, for wled_metadata.cpp
# - The output_bins script
# We always take it from package.json to ensure consistency
with open("package.json", "r") as package:
    WLED_VERSION = json.load(package)["version"]

WLED_BUILD_LABEL = f"custom {WLED_VERSION} (build {get_build_date()} {get_git_commit_short()})"

def has_def(cppdefs, name):
    """ Returns true if a given name is set in a CPPDEFINES collection """
    for f in cppdefs:
        if isinstance(f, tuple):
            f = f[0]
        if f == name:
            return True
    return False


def add_wled_metadata_flags(env, node):    
    cdefs = env["CPPDEFINES"].copy()

    if not has_def(cdefs, "WLED_REPO"):
        repo = get_github_repo()
        if repo:
            cdefs.append(("WLED_REPO", f"\\\"{repo}\\\""))

    cdefs.append(("WLED_VERSION", WLED_VERSION))

    # This transforms the node in to a Builder; it cannot be modified again
    return env.Object(
        node,
        CPPDEFINES=cdefs
    )

if not has_def(env.get("CPPDEFINES", []), "WLED_BUILD_LABEL"):
    env.Append(CPPDEFINES=[("WLED_BUILD_LABEL", f"\\\"{WLED_BUILD_LABEL}\\\"")])
   
env.AddBuildMiddleware(
    add_wled_metadata_flags,
    "*/wled_metadata.cpp"
)
