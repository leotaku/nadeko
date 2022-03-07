{ pkgs ? (import <nixpkgs> { }) }:

pkgs.llvmPackages_latest.stdenv.mkDerivation {
  name = "dev-shell";
  nativeBuildInputs = with pkgs; [ clang-tools meson ninja pkgconfig rlwrap ];
  buildInputs = with pkgs; [ libarchive sqlite ];
  strictDeps = false;
}
