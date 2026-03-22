{
  description = "Qt Inspector — MCP server, test framework, and Qt plugin for inspecting Qt/QML apps";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
  };

  outputs = { self, nixpkgs, logos-nix }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        pkgs = import nixpkgs { inherit system; };
      });
    in
    {
      packages = forAllSystems ({ pkgs }:
        let
          # MCP server built with npm dependencies
          mcpServer = pkgs.buildNpmPackage {
            pname = "qml-mcp-server";
            version = "1.0.0";
            src = ./mcp-server;
            npmDepsHash = "sha256-rDGJIt6PyXuQjO+/GsVBrTbDUIZhEeDUlOQfH8tUojM=";
            dontNpmBuild = true;
            installPhase = ''
              mkdir -p $out/lib
              cp -r . $out/lib/qml-mcp-server
            '';
          };
        in {
          # Source package: bundles qt-plugin, mcp-server (with deps), and test-framework
          # for consumption by downstream flakes (e.g. logos-basecamp).
          # The qt-plugin compiles as part of the consumer's CMake build via add_subdirectory.
          default = pkgs.runCommand "logos-qt-mcp" {} ''
            mkdir -p $out
            cp -r ${./qt-plugin} $out/qt-plugin
            cp -r ${./test-framework} $out/test-framework
            cp -r ${mcpServer}/lib/qml-mcp-server $out/mcp-server
          '';

          # Standalone MCP server binary — run with: result-mcp/bin/qml-mcp-server
          mcp-server = pkgs.writeShellScriptBin "qml-mcp-server" ''
            exec ${pkgs.nodejs}/bin/node ${mcpServer}/lib/qml-mcp-server/index.mjs "$@"
          '';
        }
      );

      devShells = forAllSystems ({ pkgs }: {
        default = pkgs.mkShell {
          nativeBuildInputs = [ pkgs.nodejs ];
          shellHook = ''
            echo "logos-qt-mcp development environment"
            echo "  MCP server: node mcp-server/index.mjs"
            echo "  Install MCP deps: cd mcp-server && npm install"
          '';
        };
      });
    };
}
