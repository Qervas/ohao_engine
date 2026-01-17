#!/usr/bin/env python3
"""
OHAO AAA Renderer Test Suite

Tests for the upgraded AAA-quality renderer including:
- Vulkan capability detection
- Feature availability
- Material system
- Render pass configuration
"""

import sys
import os

# Add the current directory to path for module import
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import ohao_renderer as renderer
except ImportError as e:
    print(f"ERROR: Could not import ohao_renderer module: {e}")
    print("Make sure to build the module first: cmake --build build --target ohao_renderer_py")
    sys.exit(1)


def test_vulkan_availability():
    """Test that Vulkan is available on the system."""
    print("\n" + "="*60)
    print("TEST: Vulkan Availability")
    print("="*60)

    available = renderer.check_vulkan_available()
    print(f"Vulkan available: {available}")

    if not available:
        print("WARNING: Vulkan not available - some tests will be skipped")

    return available


def test_renderer_capabilities():
    """Test querying renderer capabilities."""
    print("\n" + "="*60)
    print("TEST: Renderer Capabilities")
    print("="*60)

    caps = renderer.query_capabilities()
    print(caps)

    # Validate capabilities
    assert isinstance(caps.vulkan_available, bool), "vulkan_available should be bool"
    assert isinstance(caps.max_textures, int), "max_textures should be int"
    assert isinstance(caps.max_materials, int), "max_materials should be int"

    if caps.vulkan_available:
        assert len(caps.device_name) > 0, "Device name should not be empty"
        assert len(caps.vulkan_version) > 0, "Vulkan version should not be empty"
        print(f"\nDevice: {caps.device_name}")
        print(f"Vulkan Version: {caps.vulkan_version}")
        print(f"Bindless Texturing: {caps.bindless_supported}")
        print(f"Timeline Semaphores: {caps.timeline_semaphores_supported}")
        print(f"Async Compute: {caps.async_compute_supported}")

    print("PASSED: Renderer capabilities query successful")
    return caps


def test_supported_features():
    """Test listing supported renderer features."""
    print("\n" + "="*60)
    print("TEST: Supported Features")
    print("="*60)

    features = renderer.get_supported_features()

    print(f"Total features: {len(features)}")
    print("\nSupported Features:")
    for i, feature in enumerate(features, 1):
        print(f"  {i:2}. {feature}")

    # Verify key features are present
    expected_features = [
        "Deferred Rendering",
        "Screen-Space Ambient Occlusion",
        "Screen-Space Reflections",
        "Temporal Anti-Aliasing",
        "Bloom",
        "Motion Blur",
        "Depth of Field",
        "GPU-Driven Rendering",
        "Bindless Texturing",
        "Clear Coat",
        "Subsurface Scattering"
    ]

    for expected in expected_features:
        found = any(expected.lower() in f.lower() for f in features)
        if not found:
            print(f"WARNING: Expected feature not found: {expected}")

    assert len(features) >= 20, f"Expected at least 20 features, got {len(features)}"
    print(f"\nPASSED: {len(features)} features supported")
    return features


def test_material_params():
    """Test material parameter creation and modification."""
    print("\n" + "="*60)
    print("TEST: Material Parameters")
    print("="*60)

    # Create default material
    mat = renderer.MaterialParams()
    print(f"Default material: {mat}")

    # Verify default values
    assert mat.roughness == 0.5, f"Expected roughness 0.5, got {mat.roughness}"
    assert mat.metallic == 0.0, f"Expected metallic 0.0, got {mat.metallic}"
    assert mat.ao == 1.0, f"Expected ao 1.0, got {mat.ao}"

    # Modify PBR values
    mat.albedo_r = 1.0
    mat.albedo_g = 0.0
    mat.albedo_b = 0.0
    mat.roughness = 0.3
    mat.metallic = 1.0
    print(f"Red metallic material: {mat}")

    # Test advanced material features
    mat.clear_coat_intensity = 0.8
    mat.clear_coat_roughness = 0.1
    print(f"With clear coat: clear_coat={mat.clear_coat_intensity}")

    mat.subsurface_intensity = 0.5
    print(f"With subsurface: subsurface={mat.subsurface_intensity}")

    mat.anisotropy = 0.7
    print(f"With anisotropy: anisotropy={mat.anisotropy}")

    mat.transmission = 0.9
    mat.ior = 1.5
    print(f"With transmission: transmission={mat.transmission}, ior={mat.ior}")

    print("PASSED: Material parameters work correctly")
    return True


def test_enums():
    """Test renderer enum values."""
    print("\n" + "="*60)
    print("TEST: Renderer Enums")
    print("="*60)

    # Blend modes
    print("Blend Modes:")
    print(f"  OPAQUE: {renderer.BlendMode.OPAQUE}")
    print(f"  ALPHA_BLEND: {renderer.BlendMode.ALPHA_BLEND}")
    print(f"  ADDITIVE: {renderer.BlendMode.ADDITIVE}")
    print(f"  MULTIPLY: {renderer.BlendMode.MULTIPLY}")

    # Render queues
    print("\nRender Queues:")
    print(f"  BACKGROUND: {renderer.RenderQueue.BACKGROUND}")
    print(f"  GEOMETRY: {renderer.RenderQueue.GEOMETRY}")
    print(f"  ALPHA_TEST: {renderer.RenderQueue.ALPHA_TEST}")
    print(f"  TRANSPARENT: {renderer.RenderQueue.TRANSPARENT}")
    print(f"  OVERLAY: {renderer.RenderQueue.OVERLAY}")

    # Tonemap operators
    print("\nTonemap Operators:")
    print(f"  ACES: {renderer.TonemapOperator.ACES}")
    print(f"  REINHARD: {renderer.TonemapOperator.REINHARD}")
    print(f"  UNCHARTED2: {renderer.TonemapOperator.UNCHARTED2}")
    print(f"  NEUTRAL: {renderer.TonemapOperator.NEUTRAL}")

    print("PASSED: All enums accessible")
    return True


def test_version_info():
    """Test version and renderer info."""
    print("\n" + "="*60)
    print("TEST: Version Info")
    print("="*60)

    print(f"Module version: {renderer.__version__}")
    print(f"Renderer name: {renderer.RENDERER_NAME}")
    print(f"Vulkan API version: {renderer.VULKAN_API_VERSION}")

    assert renderer.__version__ == "1.0.0", f"Unexpected version: {renderer.__version__}"
    assert renderer.RENDERER_NAME == "OHAO AAA Renderer"
    assert renderer.VULKAN_API_VERSION == "1.2"

    print("PASSED: Version info correct")
    return True


def run_all_tests():
    """Run all renderer tests."""
    print("\n" + "="*60)
    print("OHAO AAA RENDERER TEST SUITE")
    print("="*60)

    results = {}

    # Run tests
    tests = [
        ("Vulkan Availability", test_vulkan_availability),
        ("Renderer Capabilities", test_renderer_capabilities),
        ("Supported Features", test_supported_features),
        ("Material Parameters", test_material_params),
        ("Renderer Enums", test_enums),
        ("Version Info", test_version_info),
    ]

    passed = 0
    failed = 0

    for name, test_func in tests:
        try:
            result = test_func()
            results[name] = "PASSED"
            passed += 1
        except Exception as e:
            results[name] = f"FAILED: {e}"
            failed += 1
            print(f"\nERROR in {name}: {e}")

    # Summary
    print("\n" + "="*60)
    print("TEST SUMMARY")
    print("="*60)

    for name, result in results.items():
        status = "✓" if result == "PASSED" else "✗"
        print(f"  {status} {name}: {result}")

    print(f"\nTotal: {passed} passed, {failed} failed out of {len(tests)} tests")

    return failed == 0


if __name__ == "__main__":
    success = run_all_tests()
    sys.exit(0 if success else 1)
