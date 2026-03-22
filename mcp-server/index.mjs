import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from "@modelcontextprotocol/sdk/types.js";
import net from "node:net";
import { Buffer } from "node:buffer";

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
const QML_HOST = process.env.QML_INSPECTOR_HOST || "localhost";
const QML_PORT = parseInt(process.env.QML_INSPECTOR_PORT || "3768", 10);
const TIMEOUT_MS = parseInt(process.env.QML_INSPECTOR_TIMEOUT || "15000", 10);

// ---------------------------------------------------------------------------
// Qt Inspector TCP bridge  (newline-delimited JSON)
// ---------------------------------------------------------------------------
class QtBridge {
  constructor() {
    this.socket = null;
    this.requestId = 0;
    this.pending = new Map(); // id -> { resolve, reject, timer }
    this.buffer = "";
  }

  async connect() {
    if (this.socket && !this.socket.destroyed) return;

    return new Promise((resolve, reject) => {
      const sock = net.createConnection({ host: QML_HOST, port: QML_PORT });

      sock.once("connect", () => {
        this.socket = sock;
        resolve();
      });

      sock.once("error", (err) => {
        reject(
          new Error(
            `Cannot connect to Qt Inspector at ${QML_HOST}:${QML_PORT} – ${err.message}. ` +
              `Make sure the Qt app is running with the inspector enabled.`
          )
        );
      });

      sock.on("data", (chunk) => {
        this.buffer += chunk.toString("utf-8");
        this._processBuffer();
      });

      sock.on("close", () => {
        this.socket = null;
        this.buffer = "";
        // Reject all pending requests
        for (const [id, p] of this.pending) {
          clearTimeout(p.timer);
          p.reject(new Error("Connection closed"));
        }
        this.pending.clear();
      });

      sock.on("error", () => {
        /* handled above on first connect; swallow subsequent */
      });
    });
  }

  _processBuffer() {
    let idx;
    while ((idx = this.buffer.indexOf("\n")) !== -1) {
      const line = this.buffer.slice(0, idx).trim();
      this.buffer = this.buffer.slice(idx + 1);
      if (!line) continue;

      try {
        const msg = JSON.parse(line);
        const id = String(msg.id);
        const p = this.pending.get(id);
        if (p) {
          clearTimeout(p.timer);
          this.pending.delete(id);
          p.resolve(msg);
        }
      } catch {
        // ignore malformed lines
      }
    }
  }

  async send(command, params = {}) {
    await this.connect();

    const id = ++this.requestId;
    const payload = JSON.stringify({ id, command, params }) + "\n";

    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(String(id));
        reject(new Error(`Request '${command}' timed out after ${TIMEOUT_MS}ms`));
      }, TIMEOUT_MS);

      this.pending.set(String(id), { resolve, reject, timer });
      this.socket.write(payload);
    });
  }
}

const bridge = new QtBridge();

// ---------------------------------------------------------------------------
// MCP Server
// ---------------------------------------------------------------------------
const server = new Server(
  { name: "qml-inspector", version: "1.0.0" },
  { capabilities: { tools: {} } }
);

// ---- Tool definitions ----

const TOOLS = [
  {
    name: "qml_find_and_click",
    description:
      "Find a UI element by its text label and click it. " +
      "This is the fastest way to interact with buttons, menu items, tabs, etc. " +
      "For example: find_and_click({text: 'Wikipedia'}) or find_and_click({text: 'Save', type: 'Button'}). " +
      "Supports case-insensitive partial matching by default.",
    inputSchema: {
      type: "object",
      properties: {
        text: {
          type: "string",
          description: "Text to search for (case-insensitive partial match by default)",
        },
        type: {
          type: "string",
          description:
            "Optional type filter (e.g. 'Button', 'Delegate'). Matches exact name or suffix.",
        },
        exact: {
          type: "boolean",
          description: "If true, require exact text match instead of partial (default false)",
        },
      },
      required: ["text"],
    },
  },
  {
    name: "qml_list_interactive",
    description:
      "List all interactive UI elements (buttons, inputs, delegates, etc.) with their labels, types, and IDs. " +
      "Use this to get an overview of what actions are available in the current UI state.",
    inputSchema: {
      type: "object",
      properties: {},
    },
  },
  {
    name: "qml_screenshot",
    description:
      "Take a screenshot of the Qt application window (or a specific widget). Returns a PNG image.",
    inputSchema: {
      type: "object",
      properties: {
        objectId: {
          type: "string",
          description: "Optional widget ID to screenshot (defaults to main window)",
        },
      },
    },
  },
  {
    name: "qml_find_by_type",
    description:
      "Find all QML/Qt objects matching a type name (exact or suffix match, e.g. 'Button' matches 'QQuickButton').",
    inputSchema: {
      type: "object",
      properties: {
        typeName: {
          type: "string",
          description: "Class/type name to search for",
        },
      },
      required: ["typeName"],
    },
  },
  {
    name: "qml_find_by_property",
    description:
      "Find all objects that have a given property, optionally matching a specific value.",
    inputSchema: {
      type: "object",
      properties: {
        property: { type: "string", description: "Property name" },
        value: { description: "Optional value to match" },
      },
      required: ["property"],
    },
  },
  {
    name: "qml_click",
    description:
      "Simulate a mouse click. Specify either (x, y) pixel coordinates relative to the main window, " +
      "or an objectId to click the center of that object.",
    inputSchema: {
      type: "object",
      properties: {
        x: { type: "number", description: "X coordinate (pixels)" },
        y: { type: "number", description: "Y coordinate (pixels)" },
        objectId: {
          type: "string",
          description: "Object ID to click (clicks center)",
        },
      },
    },
  },
  {
    name: "qml_send_keys",
    description:
      "Simulate keyboard input. Sends the given text as key press/release events to the focused widget.",
    inputSchema: {
      type: "object",
      properties: {
        text: { type: "string", description: "Text to type" },
      },
      required: ["text"],
    },
  },
  {
    name: "qml_get_properties",
    description:
      "Get all properties and methods of a specific Qt/QML object by its ID. " +
      "Returns property names, types, values, and writability.",
    inputSchema: {
      type: "object",
      properties: {
        objectId: {
          type: "string",
          description: "ID of the object (from qml_get_tree)",
        },
      },
      required: ["objectId"],
    },
  },
  {
    name: "qml_set_property",
    description:
      "Set a property value on a Qt/QML object. Use qml_get_properties first to check type and writability.",
    inputSchema: {
      type: "object",
      properties: {
        objectId: { type: "string", description: "Object ID" },
        property: { type: "string", description: "Property name" },
        value: { description: "New value (JSON-compatible)" },
      },
      required: ["objectId", "property", "value"],
    },
  },
  {
    name: "qml_call_method",
    description:
      "Call a Q_INVOKABLE method or slot on a Qt/QML object.",
    inputSchema: {
      type: "object",
      properties: {
        objectId: { type: "string", description: "Object ID" },
        method: { type: "string", description: "Method name" },
        args: {
          type: "array",
          description: "Arguments to pass",
          items: {},
        },
      },
      required: ["objectId", "method"],
    },
  },
  {
    name: "qml_evaluate",
    description:
      "Evaluate a JavaScript expression in the QML engine context. " +
      "Useful for reading computed values, calling JS functions, or debugging QML logic.",
    inputSchema: {
      type: "object",
      properties: {
        expression: {
          type: "string",
          description: "JavaScript expression to evaluate",
        },
        objectId: {
          type: "string",
          description: "Optional: evaluate in context of this object",
        },
      },
      required: ["expression"],
    },
  },
  {
    name: "qml_get_tree",
    description:
      "Get the QML/Qt object tree. Returns type, id, objectName, geometry, visibility and children. " +
      "Use this to understand the full UI structure when other tools can't find what you need.",
    inputSchema: {
      type: "object",
      properties: {
        objectId: {
          type: "string",
          description:
            "ID of the root object to start from (omit for full tree)",
        },
        depth: {
          type: "number",
          description: "Maximum depth to traverse (default 4)",
        },
      },
    },
  },
];

// ---- Handlers ----

server.setRequestHandler(ListToolsRequestSchema, async () => ({
  tools: TOOLS,
}));

server.setRequestHandler(CallToolRequestSchema, async (request) => {
  const { name, arguments: args } = request.params;

  // Map tool names to bridge commands
  const commandMap = {
    qml_get_tree: "getTree",
    qml_get_properties: "getProperties",
    qml_set_property: "setProperty",
    qml_call_method: "callMethod",
    qml_find_by_type: "findByType",
    qml_find_by_property: "findByProperty",
    qml_screenshot: "screenshot",
    qml_click: "click",
    qml_send_keys: "sendKeys",
    qml_evaluate: "evaluate",
    qml_find_and_click: "findAndClick",
    qml_list_interactive: "listInteractive",
  };

  const command = commandMap[name];
  if (!command) {
    return {
      content: [{ type: "text", text: `Unknown tool: ${name}` }],
      isError: true,
    };
  }

  try {
    const result = await bridge.send(command, args || {});

    // Handle screenshot: return image content
    if (name === "qml_screenshot" && result.image) {
      return {
        content: [
          {
            type: "image",
            data: result.image,
            mimeType: "image/png",
          },
          {
            type: "text",
            text: `Screenshot: ${result.width}x${result.height} pixels`,
          },
        ],
      };
    }

    // Handle errors from the Qt side
    if (result.error) {
      return {
        content: [{ type: "text", text: `Qt Inspector error: ${result.error}` }],
        isError: true,
      };
    }

    // Normal result
    return {
      content: [
        {
          type: "text",
          text: JSON.stringify(result, null, 2),
        },
      ],
    };
  } catch (err) {
    return {
      content: [{ type: "text", text: `Connection error: ${err.message}` }],
      isError: true,
    };
  }
});

// ---- Start ----

const transport = new StdioServerTransport();
await server.connect(transport);
