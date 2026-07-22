"""
PlatformIO Pre-Build Script for Environment Variable Injection.
Reads variables from the .env file and injects them as C++ Macros during compile time.
"""
import os

# PlatformIO injects SCons at runtime. Ignoring linter errors for these dynamic imports.
try:
    from SCons.Script import Import  # type: ignore
except ImportError:
    pass

# Import the PlatformIO build environment globally
Import("env")  # type: ignore # noqa: F821

def load_env_vars(env_file_path=".env"):
    """
    Parses a .env file and returns a dictionary of key-value pairs.
    """
    env_vars = {}
    
    try:
        from dotenv import load_dotenv
        load_dotenv(env_file_path)
        
        keys_to_extract = ["AWS_IOT_ENDPOINT", "THING_NAME", "FW_VERSION"]
        for key in keys_to_extract:
            val = os.getenv(key)
            if val:
                env_vars[key] = val
                
    except ImportError:
        print("[WARNING] 'python-dotenv' not found. Falling back to manual .env parsing.")
        if os.path.exists(env_file_path):
            with open(env_file_path, "r") as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#"):
                        continue
                    if "=" in line:
                        key, val = line.split("=", 1)
                        env_vars[key.strip()] = val.strip().strip('"').strip("'")
        else:
            print(f"[ERROR] '{env_file_path}' file not found in the root directory!")
            
    return env_vars

def inject_macros():
    """
    Injects extracted environment variables into the PlatformIO build environment.
    """
    print("[INFO] Executing pre-build script: apply_env.py")
    env_vars = load_env_vars()
    
    if not env_vars:
        print("[WARNING] No environment variables found to inject.")
        return

    macro_flags = []
    for key, value in env_vars.items():
        macro_flags.append((key, f'\\"{value}\\"'))
        print(f"[INFO] Injected Macro -> {key}: {value}")
    
    # Inject into the global 'env' provided by PlatformIO
    global env  # type: ignore # noqa: F821
    env.Append(CPPDEFINES=macro_flags)  # type: ignore # noqa: F821

if __name__ == "SCons.Script":
    inject_macros()