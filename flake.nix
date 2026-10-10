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

      # nix build .#checks.<system>.click-order -L
      checks = forAllSystems ({ pkgs }: {
        click-order = pkgs.stdenv.mkDerivation {
          name = "qml-inspector-click-order";
          src = ./.;
          cmakeDir = "../tests";
          nativeBuildInputs = [ pkgs.cmake pkgs.nodejs ];
          buildInputs = [ pkgs.qt6.qtbase pkgs.qt6.qtdeclarative ];
          dontWrapQtApps = true;
          # The test talks to the app over loopback TCP.
          __darwinAllowLocalNetworking = true;
          doCheck = true;
          checkPhase = ''
            export HOME=$TMPDIR
            export QT_PLUGIN_PATH=${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}
            export QML_IMPORT_PATH=${pkgs.qt6.qtdeclarative}/${pkgs.qt6.qtbase.qtQmlPrefix}
            export QML_INSPECTOR_PORT=37681  # not 3768, which a running app may hold
            node ../tests/click-order.mjs --ci ./inspector-test-app
          '';
          installPhase = "touch $out";
        };
      });

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
