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
        println!("1. Copy the PC private_key to config.toml [security] section");
        println!("2. Copy the Cardputer public_key to config.toml as cardputer_public_key");
        println!("3. Copy the Cardputer private_key to your Cardputer firmware config");
        println!("4. Copy the PC public_key to your Cardputer firmware as server_public_key");
        if gen_cookie {
            println!("5. Copy the same discovery_cookie to both PC and Cardputer configs");
        }
    }

    println!("\n⚠️  SECURITY WARNING:");
    println!("   - Keep private keys SECRET - never share them!");
    println!("   - Store them securely on each device");
    println!("   - Generate new keys if compromised");
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
