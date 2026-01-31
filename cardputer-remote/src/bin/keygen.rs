//! Key generation utility for Cardputer Remote
//!
//! Generates ECDH keypairs for secure communication.
//!
//! Usage:
//!   keygen [OPTIONS]
//!
//! Options:
//!   --pc          Generate keypair for PC (server)
//!   --cardputer   Generate keypair for Cardputer (client)
//!   --both        Generate keypairs for both (default)
//!   --cookie      Generate a random discovery cookie
//!   -h, --help    Show help

use p256::ecdsa::SigningKey;
use rand::rngs::OsRng;

fn main() {
    let mut args = std::env::args().skip(1);
    let mut gen_pc = false;
    let mut gen_cardputer = false;
    let mut gen_cookie = false;

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--pc" => gen_pc = true,
            "--cardputer" => gen_cardputer = true,
            "--both" => {
                gen_pc = true;
                gen_cardputer = true;
            }
            "--cookie" => gen_cookie = true,
            "-h" | "--help" => {
                print_help();
                return;
            }
            _ => {
                eprintln!("Unknown argument: {}", arg);
                print_help();
                std::process::exit(1);
            }
        }
    }

    // Default to both if nothing specified
    if !gen_pc && !gen_cardputer && !gen_cookie {
        gen_pc = true;
        gen_cardputer = true;
        gen_cookie = true;
    }

    println!("=== Cardputer Remote Key Generator ===\n");

    if gen_cookie {
        generate_cookie();
        println!();
    }

    if gen_pc {
        println!("--- PC (Server) Keypair ---");
        generate_keypair("PC");
        println!();
    }

    if gen_cardputer {
        println!("--- Cardputer (Client) Keypair ---");
        generate_keypair("Cardputer");
        println!();
    }

    println!("=== Configuration Instructions ===\n");

    if gen_pc && gen_cardputer {
        println!("PC (Server) - config.toml:");
        println!("  [security]");
        println!("  private_key = \"<PC Private Key>\"");
        println!("  cardputer_public_key = \"<Cardputer Public Key>\"\n");

        println!("Cardputer (ESP32) - SD card /rd_keys/ directory:");
        println!("  client.key  - Cardputer private key (32 bytes binary)");
        println!("  client.pub  - Cardputer public key (33 bytes binary)");
        println!("  server.pub  - PC public key (33 bytes binary)\n");

        println!("To create binary key files for ESP32:");
        println!("  echo '<hex>' | xxd -r -p > /sd/rd_keys/client.key");
        println!("  echo '<hex>' | xxd -r -p > /sd/rd_keys/client.pub");
        println!("  echo '<hex>' | xxd -r -p > /sd/rd_keys/server.pub\n");

        if gen_cookie {
            println!("Discovery cookie goes in both configs (same value).");
        }
    }

    println!("⚠️  SECURITY WARNING:");
    println!("   - Keep private keys SECRET - never share them!");
    println!("   - Store them securely on each device");
    println!("   - Generate new keys if compromised");
    println!("   - NEVER commit keys to version control!");
}

fn print_help() {
    println!("Cardputer Remote Key Generator");
    println!();
    println!("Usage: keygen [OPTIONS]");
    println!();
    println!("Options:");
    println!("  --pc          Generate keypair for PC (server)");
    println!("  --cardputer   Generate keypair for Cardputer (client)");
    println!("  --both        Generate keypairs for both (default)");
    println!("  --cookie      Generate a random discovery cookie");
    println!("  -h, --help    Show this help");
}

fn generate_keypair(name: &str) {
    let signing_key = SigningKey::random(&mut OsRng);
    let verifying_key = signing_key.verifying_key();

    // Get private key bytes
    let private_bytes = signing_key.to_bytes();
    let private_hex = hex::encode(private_bytes);

    // Get compressed public key
    let public_point = verifying_key.to_encoded_point(true);
    let public_hex = hex::encode(public_point.as_bytes());

    println!("{} Private Key (32 bytes, keep secret!):", name);
    println!("  {}", private_hex);
    println!();
    println!("{} Public Key (33 bytes compressed, share with peer):", name);
    println!("  {}", public_hex);
}

fn generate_cookie() {
    use rand::RngCore;

    let mut cookie = [0u8; 16];
    OsRng.fill_bytes(&mut cookie);

    println!("Discovery Cookie (16 bytes, same on both devices):");
    println!("  {}", hex::encode(cookie));
}
