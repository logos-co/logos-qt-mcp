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
      packages = forAllSystems ({ pkgs }: {
        # Source package: bundles qt-plugin, mcp-server, and test-framework
        # for consumption by downstream flakes (e.g. logos-basecamp).
        # The qt-plugin compiles as part of the consumer's CMake build via add_subdirectory.
        default = pkgs.runCommand "logos-qt-mcp" {} ''
          mkdir -p $out
          cp -r ${./qt-plugin} $out/qt-plugin
          cp -r ${./mcp-server} $out/mcp-server
          cp -r ${./test-framework} $out/test-framework
        '';
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
