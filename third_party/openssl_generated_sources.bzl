# Generate OpenSSL assembly files from perlasm inputs.

crypto_asm_inputs_linux_x86_64 = [
    "crypto/x86_64cpuid.pl",
    "crypto/aes/asm/aes-x86_64.pl",
    "crypto/modes/asm/aesni-gcm-x86_64.pl",
    "crypto/aes/asm/aesni-mb-x86_64.pl",
    "crypto/aes/asm/aesni-sha1-x86_64.pl",
    "crypto/aes/asm/aesni-sha256-x86_64.pl",
    "crypto/aes/asm/aesni-x86_64.pl",
    "crypto/aes/asm/bsaes-x86_64.pl",
    "crypto/aes/asm/vpaes-x86_64.pl",
    "crypto/bn/asm/x86_64-mont.pl",
    "crypto/bn/asm/x86_64-mont5.pl",
    "crypto/ec/asm/ecp_nistz256-x86_64.pl",
    "crypto/md5/asm/md5-x86_64.pl",
    "crypto/modes/asm/ghash-x86_64.pl",
    # We don't really care about RC4 asm but there doesn't appear to be an easy
    # way to disable it individuallly.
    "crypto/rc4/asm/rc4-x86_64.pl",
    "crypto/rc4/asm/rc4-md5-x86_64.pl",
    "crypto/sha/asm/sha1-x86_64.pl",
    "crypto/sha/asm/sha1-mb-x86_64.pl",
    # sha256-x86_64.pl is a symlink to sha512-x86_64.pl.
    # Perlasm tries to detect the SHA flavour automatically from the output
    # file name, so an invocation with sha256-x86_64.S generates SHA-256 and
    # an invocation with sha512-x86_64.S generates SHA-512. (We just hope that
    # blaze paths do not match *256* or *512*.)
    "crypto/sha/asm/sha256-x86_64.pl",
    "crypto/sha/asm/sha256-mb-x86_64.pl",
    "crypto/sha/asm/sha512-x86_64.pl",
    "crypto/bn/asm/rsaz-avx2.pl",
    "crypto/bn/asm/rsaz-x86_64.pl",
]

def gen_asm_src(input):
  """Generate a single source with a custom output name."""

  output = input.replace(".pl", ".S")
  genrule_name = "%s_gen" % output.split("/")[-1]

  if output == "crypto/sha/asm/sha256-x86_64.S":
    input = "crypto/sha/asm/sha512-x86_64.pl"

  cmd = ("out=$(location %s);" + "perl $(location %s) elf $$out;") % (output,
                                                                      input)

  native.genrule(
      name=genrule_name,
      srcs=[input, "crypto/perlasm/x86_64-xlate.pl"],
      outs=[output],
      cmd=cmd,
      visibility=["//visibility:private"],)
  return output

def gen_asm_srcs(flavour):
  """Generate .s files from perlasm inputs."""

  inputs = {"linux_x86_64": crypto_asm_inputs_linux_x86_64}

  return [gen_asm_src(f) for f in inputs[flavour]]
