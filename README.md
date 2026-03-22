# QML Inspector — MCP Server & Test Framework

A toolkit for inspecting, interacting with, and testing Qt/QML applications. It consists of:

1. **Qt Plugin** (`qt-plugin/`) — a C++ inspector server that embeds in the Qt app and exposes a TCP API
2. **MCP Server** (`mcp-server/`) — a bridge that exposes the inspector as [Model Context Protocol](https://modelcontextprotocol.io) tools for AI assistants (Claude, etc.)
3. **Test Framework** (`test-framework/`) — a reusable library for writing UI tests against any Qt app using the inspector

## Architecture

```
┌─────────────┐     TCP/JSON      ┌──────────────┐     stdio/MCP      ┌─────────────┐
│  Qt App +    │◄────────────────►│  MCP Server  │◄──────────────────►│  Claude /    │
│  Inspector   │   port 3768      │  (index.mjs) │                    │  AI Agent    │
│  Plugin      │                  └──────────────┘                    └─────────────┘
│              │     TCP/JSON      ┌──────────────┐
│              │◄────────────────►│  Test Runner  │
│              │   port 3768      │  (your tests) │
└─────────────┘                   └──────────────┘
```

## Directory Structure

```
logos-qt-mcp/                    # This repo
  qt-plugin/                     # C++ inspector that embeds in Qt apps
    inspectorserver.h
    inspectorserver.cpp
    CMakeLists.txt
  mcp-server/                    # MCP bridge for AI agents
    index.mjs
    package.json
  test-framework/                # Reusable test library
    framework.mjs
```

App-specific tests stay in the app repo (e.g. `logos-basecamp/tests/ui-tests.mjs`) and import the framework from this repo.

## Qt Plugin

### Setup

Add the inspector to your Qt app:

```cpp
#include "inspectorserver.h"

// In your main window setup:
InspectorServer::attach(mainWindow);  // listens on port 3768
```

The port can be configured via the `QML_INSPECTOR_PORT` environment variable.

### What it provides

The inspector exposes the full Qt object tree over TCP using a newline-delimited JSON protocol. It can:

- Traverse the widget/QML object tree
- Read/write properties on any object
- Call methods and slots
- Simulate mouse clicks and keyboard input
- Take screenshots
- Evaluate QML/JavaScript expressions
- Find elements by type, property, or text

### Tree enrichment

The object tree includes common identifying properties (`text`, `title`, `source`, `label`, etc.) directly in each node, so consumers don't need extra round-trips to identify elements.

## MCP Server

The MCP server lets AI assistants interact with the running Qt app.

### Configuration

Add to your `.mcp.json`:

```json
{
  "mcpServers": {
    "qml-inspector": {
      "command": "node",
      "args": ["/path/to/logos-qt-mcp/mcp-server/index.mjs"],
      "env": {
        "QML_INSPECTOR_HOST": "localhost",
        "QML_INSPECTOR_PORT": "3768"
      }
    }
  }
}
```

### Available tools

#### Low-level tools

| Tool | Description |
|------|-------------|
| `qml_get_tree` | Get the object tree with type, id, text, geometry, visibility |
| `qml_get_properties` | Get all properties and methods of an object |
| `qml_set_property` | Set a property value on an object |
| `qml_call_method` | Call a method or slot on an object |
| `qml_find_by_type` | Find objects by class name |
| `qml_find_by_property` | Find objects by property name/value |
| `qml_screenshot` | Take a PNG screenshot |
| `qml_click` | Click at coordinates or on an object |
| `qml_send_keys` | Send keyboard input |
| `qml_evaluate` | Evaluate JavaScript in the QML engine |

#### High-level tools

| Tool | Description |
|------|-------------|
| `qml_find_and_click` | Find an element by text and click it in one call |
| `qml_list_interactive` | List all visible interactive elements (buttons, inputs, etc.) |

### Usage examples

```
# One call to open an app:
qml_find_and_click({text: "webview_app"})

# Find and click with type filter:
qml_find_and_click({text: "Save", type: "Button"})

# See what's clickable:
qml_list_interactive()
```

## Test Framework

A reusable Node.js library for writing UI tests. Tests connect directly to the Qt Inspector (no MCP server needed).

### Writing tests

Create a test file that imports from the framework:

```javascript
import { test, run } from "/path/to/logos-qt-mcp/test-framework/framework.mjs";

test("my app: verify buttons", async (app) => {
  await app.click("my_app");
  await app.expectTexts(["Save", "Cancel"]);
});

test("my app: click save", async (app) => {
  await app.click("my_app");
  await app.click("Save");
  await app.expectTexts(["Saved successfully"]);
});

run();
```

### Running tests

```bash
# Run all tests (app must already be running):
node tests/ui-tests.mjs

# Run tests matching a filter:
node tests/ui-tests.mjs counter

# CI mode — launches the app, runs tests, kills the app:
node tests/ui-tests.mjs --ci /path/to/logos-basecamp
node tests/ui-tests.mjs --ci /path/to/logos-basecamp counter
```

### Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `QML_INSPECTOR_HOST` | `localhost` | Inspector host |
| `QML_INSPECTOR_PORT` | `3768` | Inspector port |

### Test API

#### Actions

| Method | Description |
|--------|-------------|
| `app.click(text, opts?)` | Find element by text and click it. Options: `type` (filter by class name), `exact` (require exact match) |
| `app.screenshot()` | Take a screenshot (returns base64 PNG) |

#### Assertions

| Method | Description |
|--------|-------------|
| `app.expectTexts(["a", "b"])` | Assert elements with these text values exist in the UI |
| `app.expectStatus(text)` | Assert a status label contains the given text |
| `app.expectProperty(type, prop, value)` | Assert a property value on an object found by type |

#### Queries

| Method | Description |
|--------|-------------|
| `app.getTree(opts?)` | Get the object tree |
| `app.listInteractive()` | List all interactive elements |
| `app.findByProperty(prop, value?)` | Find objects by property |
| `app.getProperties(objectId)` | Get all properties of an object |
| `app.getPropertyByType(type, prop)` | Read a property from the first object matching a type |

#### Utilities

| Method | Description |
|--------|-------------|
| `app.waitFor(fn, opts?)` | Poll until a condition passes. Options: `timeout` (ms, default 5000), `interval` (ms, default 300) |

### Running in CI with Nix

The integration tests are available as a Nix derivation:

```bash
# Build and run:
nix build .#integration-test -L

# macOS app bundle variant:
nix build .#integration-test-bundle -L

# View the log:
cat result/integration-test.log
```

Add to GitHub Actions:

```yaml
- name: Integration test
  run: |
    nix build .#integration-test --out-link result-integration -L
    cat result-integration/integration-test.log
```

The CI mode launches the app with `-platform offscreen` (no display needed), waits for the inspector to become available, runs all tests, and exits with a non-zero code if any test fails.

## Using in your Qt app

To use this toolkit in another Qt app:

1. **Link the Qt plugin** — point `LOGOS_QT_MCP_ROOT` at this repo and use `add_subdirectory` in CMake
2. **Reference the MCP server** in `.mcp.json` for AI agent interaction
3. **Import the test framework** in your test files:
   ```javascript
   import { test, run } from "/path/to/logos-qt-mcp/test-framework/framework.mjs";
   ```
