{ pkgs ? (import <nixpkgs> { }) }:

pkgs.llvmPackages_latest.stdenv.mkDerivation {
  name = "dev-shell";
  nativeBuildInputs = with pkgs; [ clang-tools meson ninja pkg-config rlwrap ];
  buildInputs = with pkgs; [ libarchive sqlite ];
  strictDeps = false;
}
