# Physics Testing with Python + pybind11

## Why Python Testing?

**Modern physics engines (UE5, Unity, PhysX) use automated testing.** Python makes this easy:

- ✅ **No recompile** - instant iteration
- ✅ **Self-validating** - laws of physics are ground truth
- ✅ **ML/AI ready** - integrate with PyTorch, JAX, etc.
- ✅ **Data visualization** - plot energy, trajectories
- ✅ **CI/CD friendly** - runs in GitHub Actions

## Setup

### 1. Install pybind11

```bash
# macOS
brew install pybind11

# Or via pip
pip install pybind11[global]
```

### 2. Build Python Bindings

```bash
cd build
cmake .. -DBUILD_PYTHON_BINDINGS=ON
make ohao_physics
```

This creates `ohao_physics.cpython-*.so` module.

### 3. Install Test Dependencies

```bash
pip install pytest numpy matplotlib
```

## Running Tests

### Run All Tests

```bash
cd tests/python
pytest -v
```

### Run Specific Test Category

```bash
# Physical invariants (energy, momentum, etc.)
pytest test_physics_invariants.py -v

# Analytical solutions (free fall, collisions, etc.)
pytest test_analytical_solutions.py -v
```

### Run Single Test

```bash
pytest test_physics_invariants.py::TestEnergyConservation::test_free_fall_energy_conservation -v
```

## Test Categories

### 1. **Physical Invariants** (Laws that MUST hold)

- **Energy Conservation** - Total energy constant (or decreases with friction)
- **Momentum Conservation** - Total momentum constant in isolated systems
- **No Penetration** - Objects never interpenetrate significantly
- **Stability** - Stable configurations remain stable

**Example:**
```python
def test_energy_conservation():
    world = physics.PhysicsWorld()
    ball = world.create_rigid_body_with_sphere(radius=0.5, position=(0, 10, 0))

    initial_energy = calculate_total_energy(world)

    for _ in range(120):  # 2 seconds
        world.step(1/60)
        current_energy = calculate_total_energy(world)
        assert current_energy <= initial_energy  # Energy never increases!
```

### 2. **Analytical Solutions** (Compare to exact math)

- **Free Fall** - h = h0 - 0.5*g*t²
- **Projectile Motion** - Range formula
- **Elastic Collision** - Velocity exchange formulas
- **Restitution** - v_separation = -e * v_approach

**Example:**
```python
def test_free_fall():
    ball.set_position((0, 100, 0))

    for t in range(120):
        world.step(1/60)
        expected_y = 100 - 0.5 * 9.81 * (t/60)**2
        actual_y = ball.get_position().y
        assert abs(actual_y - expected_y) < 0.1  # Within 10cm
```

## Interactive Testing (Jupyter)

```bash
jupyter notebook
```

```python
# In Jupyter cell
import ohao_physics as physics
import matplotlib.pyplot as plt

world = physics.PhysicsWorld()
ball = world.create_rigid_body_with_sphere(0.5, position=(0, 10, 0))

positions = []
for _ in range(120):
    world.step(1/60)
    positions.append(ball.get_position().y)

plt.plot(positions)
plt.title("Ball Falling")
plt.xlabel("Frame")
plt.ylabel("Height (m)")
plt.show()
```

## ML/AI Integration Example

```python
import torch
import ohao_physics as physics

# Use physics as RL environment
env = physics.PhysicsWorld()

for episode in range(1000):
    state = env.reset()
    for step in range(100):
        action = policy_network(state)
        next_state, reward = env.step(action)
        # Train neural network...
```

## Continuous Integration (GitHub Actions)

```yaml
# .github/workflows/physics-tests.yml
name: Physics Tests
on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - uses: actions/setup-python@v2
      - run: pip install pytest numpy pybind11
      - run: cmake -B build -DBUILD_PYTHON_BINDINGS=ON
      - run: cmake --build build --target ohao_physics
      - run: cd tests/python && pytest -v
```

## Debugging Failed Tests

```bash
# Run with detailed output
pytest -vvs

# Drop into debugger on failure
pytest --pdb

# Run only failed tests
pytest --lf
```

## Writing New Tests

**Rule of thumb:**
- Use **invariants** for general correctness (energy, momentum)
- Use **analytical** for specific scenarios (free fall, collisions)

**Template:**
```python
def test_my_scenario():
    """Clear description of what's being tested"""
    world = physics.PhysicsWorld()

    # Setup
    obj = world.create_rigid_body_with_box(...)

    # Simulate
    for _ in range(frames):
        world.step(1/60)

    # Assert (use physics laws as ground truth)
    assert obj.kinetic_energy <= initial_energy
```

## Benefits Over Manual Testing

| Manual Testing | Automated Python Tests |
|----------------|------------------------|
| Open engine → click Start → watch | `pytest` |
| 30 seconds per test | 0.1 seconds per test |
| Must visually inspect | Self-validating (pass/fail) |
| Can't test edge cases | Test 1000s of scenarios |
| No regression detection | Catches regressions instantly |
| Requires full engine | Headless (no GPU/window) |

## Next Steps

1. **Build the bindings**: `make ohao_physics`
2. **Run tests**: `pytest tests/python -v`
3. **Fix failing tests** - each failure points to a physics bug!
4. **Add more tests** - expand coverage

The laws of physics are the ultimate ground truth! 🌟
