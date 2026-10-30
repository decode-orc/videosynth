# Testing vision and strategy

## Common

### Vision

Good software should have the following traits:
- It is easy to modify/maintain
- It is easy to test quickly and automatically
- It is easy to use

### Strategy to implement vision

#### Architecture

```
MONOLITH PATTERN                    DEPENDENCY INVERSION PATTERN
  ┌─────────────────────┐             ┌────────────────┐   ┌──────────────────────┐   ┌──────────────────────┐
  │                     │             │                │   │  «Interface»         │   │  «Interface»         │
  │  ┌───────────────┐  │             │                ├──►│  public method B()   │   │  public method C()   │
  │  │ public        │  │             │  Class         │   ├──────────────────────┤   ├──────────────────────┤
  │  │ method A()    │  │             │  public        │   │  Class               ├──►│  Class               │
  │  ├───────────────┤  │    ════►    │  method A()    │   │  public method B()   │   │  public method C()   │
  │  │ private       │  │             │                │   └──────────────────────┘   └──────────────────────┘
  │  │ method B()    │  │             │                │
  │  ├───────────────┤  │             └────────────────┘
  │  │ private       │  │
  │  │ method C()    │  │                  class 1          class 2 (with interface)    class 3 (with interface)
  │  └───────────────┘  │
  │                     │
  │  monolithic class 1 │
  └─────────────────────┘
  ```

Instead of writing monolithic classes (as in the above example), we architect classes that are smaller and depend upon abstractions (interfaces) rather than concrete objects.
In C++, an interface is a class composed entirely of pure virtual methods.

Depending upon abstractions instead of concrete objects is known as the Dependency Inversion design pattern.

##### How classes receive their dependencies
Instead of a class calling the constructor of one of its dependencies (and thus having a tightly coupled link),
we *inject* the abstract dependencies (interfaces) into the class constructor.
Formal dependency injection frameworks tend to work better when only interfaces are injected into a constructor,
therefore having a separate *init* method may be wise for non-interface dependencies.

**Important**: The init method should not appear in an interface, only the concrete class.  It's an implementation detail, so only code that handles instantiation (ie factories) should know about it.

**Caveat**: If using this 'init method' convention, you may have to use raw pointers instead of referencing objects (ie ISomeObject* instead of ISomeObject&) due to the compiler not allowing the object to temporarily be null before init is called.

##### std::shared_ptr<ISomeInterface> vs using ISomeInterface&
It's more convenient to just pass around references to interfaces (including for testing), so I prefer this by default.
But if there's a question of an interface going out of scope (and thus being de-allocated) prematurely, then I wrap the interface in std::shared_ptr to be safe.

##### What if the class needs to instantiate an object dynamically, such as opening a file?
In this case, follow the Abstract Factory design pattern: define an interface with methods that return more interfaces.
Any object created by such an interface should be wrapped in std::shared_ptr to ensure it doesn't get de-allocated prematurely.

#### Testing architecture

```
INTEGRATION TEST PATTERN                    UNIT TEST PATTERN
  ┌──────────────────────────┐               ┌─────────────────────┐   ┌─────────────────────┐   ┌─────────────────────┐
  │ Integration test for     │               │ Unit test for       │   │ Unit test for       │   │ Unit test for       │
  │ method C. Must also      │               │ method A            │   │ method B            │   │ method C            │
  │ test method A and B.     │               └──────────┬──────────┘   └──────────┬──────────┘   └──────────┬──────────┘
  └────────────┬─────────────┘                          │                         │                         │
               │                                        ▼                         ▼                         ▼
               ▼                              ┌─────────────────────┐   ┌─────────────────────┐   ┌─────────────────────┐
  ┌────────────────────────┐                  │  «Class»            │   │  «Class»            │   │  «Class»            │
  │  ┌──────────────────┐  │    ════►         │  public method A()  │   │  public method B()  │   │  public method C()  │
  │  │ public           │  │                  └──────────┬──────────┘   └──────────┬──────────┘   └─────────────────────┘
  │  │ method A()       │  │                             │                         │
  │  ├──────────────────┤  │                             ▼                         ▼
  │  │ private          │  │                  ┌─────────────────────┐   ┌─────────────────────┐
  │  │ method B()       │  │                  │ Mock of interface   │   │ Mock of interface   │
  │  ├──────────────────┤  │                  │ containing          │   │ containing method   │
  │  │ private          │  │                  │ method B()          │   │ C()                 │
  │  │ method C()       │  │                  └─────────────────────┘   └─────────────────────┘
  │  └──────────────────┘  │
  │                        │                   unit test for only        unit test for only       unit test for only
  │  complicated,          │                   method A                  method B                 method C
  │  hard-to-maintain test │
  └────────────────────────┘

```

##### Encapsulation

Get into the mindset of not trying to test your dependencies' internal implementations.  You give your dependencies inputs and expect certain outputs as a result.
It's not your job to test your dependencies' internal implementations or even know what those implementations are.  Trying to do so leads to tight coupling and bad architecture.

Staying intentionally ignorant of how dependencies are implemented internally is known as encapsulation.  Practicing this discipline makes your project easy to modify/maintain.  Any module is free to change its implementation without breaking the rest of the project.
Modules depend upon stable abstract interfaces rather than concrete classes.

##### Unit tests
Unit tests are small, lightweight tests that test only a small bit of code, usually a single method.

Benefits to having a large suite of unit tests:
- They execute extremely quickly (thousands of tests can be run in seconds)
- They are effective at digging into corner cases that would otherwise be difficult (or slow) to automatically test
- They encourage encapsulation in the project architecture, which tends to make the project easier to modify/maintain
- They are self-contained so don't require any 'tribal knowledge' to set up
- If they fail, it's really easy to pinpoint the source of the problem

Rules for unit tests:
- All dependencies are mocked out. They never touch the file system, the network, a database, or the system clock. This makes them deterministic which is very desirable.
- They usually only test a single method.  If a method calls a small private method, this forces your test to test two methods.  This is usually okay if the private method is very simple, but large private methods are an anti-pattern and should be moved to another class/interface.
- My rule of thumb is that unit tests should make up 80% of all tests for a well-designed project.

##### Integration tests
Integration tests test multiple methods, possibly even multiple classes together, to get an overall idea of whether a section of the project works.

They tend to execute slowly, are poor at digging into corner cases, and tend to be bigger (as in lines of code) and harder to maintain.

I tend to use them to test a few 'happy path' cases, just to ensure that everything is wired up correctly.
Unit tests aren't great at 'big picture' stuff, so having a few integration tests, possibly even end-to-end tests, is handy.

Integration tests may test the file system, network, database, clock, etc, so are harder to troubleshoot if anything goes wrong,
and may be harder for a newcomer to set up in their environment.

##### Mocks

Mocks are classes that implement interfaces, designed specifically for unit testing.

They return expected results from methods when expected arguments are passed into those methods.
They specifically do NOT try to implement any real functionality.
Methods should be designed to not care how their dependencies are implemented; this is crucial to maintain good encapsulation and keep your project easy to modify/maintain.

For example, a mock of a *multiply* method would return 12 when it receives 3 and 4 as inputs.  It would not actually multiply 3 and 4 and return the result.  Going down the road to implementing real multiplication in a test like this is going down the road to tight coupling.  Discipline yourself to not try to test your dependencies' implementations.

One can make ad-hoc mocks from scratch, but it's a lot more efficient to use a formal framework.

For C++, the one I recommend is Google Test (which includes a mocking framework).

###### Should I mock a class or an interface?

Technically, Google Test does support mocking a class instead of an interface.

However, I discourage this practice because when one mocks an interface, it's impossible for side effects to creep through in tests.
If one mocks a class, side effects are possible (ie if one forgets to override a virtual method in the base class).

## Determinism requirements

- Unit tests must remain deterministic and isolated from network/clock side effects.
- Functional tests may use filesystem operations for install/relocation verification, but must remain deterministic and self-contained.
- Runtime resource lookup failure behavior must produce deterministic hard-fail diagnostics.

## Repository layout

`tests/` holds automated tests and nothing else. Project YAML lives in
`projects/`; generated media never lands in either tree.

```
tests/
├── CMakeLists.txt        Owns every test target
├── support/              Shared helpers: fixture_paths.h, efm_channel_decoder.h
├── unit/                 → videosynth_unit_tests,           ctest label "unit"
├── functional/           → videosynth_functional_tests,     label "functional"
└── gui/
    ├── support/          Shared QCoreApplication entry point
    ├── unit/             → videosynth_gui_unit_tests,       label "unit"
    └── functional/       → videosynth_gui_functional_tests, label "functional"
```

**Classification is the directory, not a list.** A source file under `unit/` is
built into the unit binary and every test it declares is labelled `unit`; the
same holds for `functional/`. Adding a test file is enough — there is no
pattern list to keep in step. A file whose tests split across both categories
must be split into two files, one per tree.

Run one lane with `ctest -L unit` or `ctest -L functional`. The `unit` lane is
the fast, mocked, hermetic one and is the only lane the packaged Nix build
runs; keep it that way.

## Project fixtures

`projects/` holds only hand-authored projects:

| Directory | Contents |
|-----------|----------|
| `projects/general/` | Feature fixtures (audio, EFM, VITS, progressive sources, 2:3 pulldown film) — composite |
| `projects/general-yc/` | Feature fixtures — Y/C output |
| `projects/stacking/` | Disc-simulation, skip, and stacking fixtures |
| `projects/long-form/` | Capture-sized three-hour examples, run by hand only |
| `projects/benchmark/` | Fixed-length performance projects for `scripts/benchmark.sh` |
| `projects/variants.json` | Rules for the mechanically derived variants |

Anything mechanically derivable is generated at build time by
`scripts/generate_test_projects.py` into `build/generated-projects/` and is not
committed — currently the `stacking-captures` set, the impairment-free
`stacking-clean` set, the Y/C `stacking-yc` set and the CLV and PAL-M film
sets `film-clv` and `film-pal-m`. Add a variant by editing `variants.json`, not by copying
YAML. Generation needs a Python 3 interpreter; without one the build still
succeeds and the suites simply cover the committed fixtures alone.

Fixtures write through the `{output}` logical asset root, so a run's target
directory is the caller's choice:

- `scripts/run-projects.sh [general|stacking]` runs a suite end to end through
  the CLI into `build/project-output/<suite>/`.
- `scripts/output-hashes.sh` regenerates those suites and compares SHA-256
  manifests of the generated media against a recorded baseline
  (`--record` to record one), which is how a change is asserted to be
  byte-identical to the previous behaviour.
- `scripts/benchmark.sh` times `projects/benchmark/` per thread configuration
  into `build/project-output/benchmark/`.
- The functional suites point `{output}` at a scratch directory, so
  `ctest -L functional` never writes into the source tree.
- A bare `videosynth --project projects/general/pal_vits.yaml` writes beside
  the YAML, which is what you usually want when running one by hand.
