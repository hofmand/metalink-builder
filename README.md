# dir2ml
A command line tool to create metalink ([RFC5854](https://tools.ietf.org/html/rfc5854)) files (`.meta4`/`.metalink`) from a directory structure on local storage, complete with multiple cryptographic hash options. The latest binary release for Windows x86 and x64 can be found [here](https://github.com/hofmand/dir2ml/releases).

This tool, `dir2ml`, will create a single UTF-8 metalink file from a supplied directory and base URL, with one `file` record per file, sorted purely [ASCIIbetically](https://en.wiktionary.org/wiki/ASCIIbetical) by path. The metalink file is an XML format so it can be rendered in a variety of ways using ordinary tools such as XML transformation utilities operating on XML stylesheets (`.xslt`). See an [example](#example-output-file) of this file format below.

Later, when the original storage location of the files goes offline, the `.meta4` file can be used to identify and locate those files by hash on other servers or by P2P *(try [aria2](https://aria2.github.io/))*, and to reconstruct the original file structures.

Try it! Run `dir2ml.exe` on your own computer's download directory, open the `.meta4` file it generates, and search on your favorite search engine for some of the hashes. *You might have the best luck with MD5 hashes of `.tar.gz` files but as metalinks become more ubiquitous, it will become easier to locate other lost files.*

Another use-case is to use `dir2ml` to periodically [fingerprint](https://www.technologyreview.com/s/402961/fingerprinting-your-files/) your hard drive onto a USB flash drive *(I recommend you use `--sparse-output --file-url` for that purpose)*, then if your hard drive starts to crash or if you're hit by ransomware, you can use any ordinary diff tool to compare two `.meta4` files and easily determine which files have changed and need to be restored from [backup](https://www.backblaze.com/blog/the-3-2-1-backup-strategy/). (*You have a backup, right?*)

---

## Table of Contents
* [Usage](#usage)
* [Example Output File](#example-output-file)
* [Limitations](#limitations)
* [Future Plans](#future-plans)
* [Built With](#built-with)
* [Authors](#authors)
* [License](#license)

---

## Install

1. Clone this repository:

   `git clone --recursive https://github.com/hofmand/metalink-builder.git`

   or use your web browser to download the `.zip` file, then unpack it to the directory of your choice.

2. Compile `metalink-builder\src\metalink-tools.sln`

3. `dir2ml.exe` will be located in `metalink-builder\src\x64\Release` or `metalink-builder\src\x64\Debug`

## Usage

### Show Help

**`dir2ml --help`**

### Minimal Parameters

**`dir2ml -d`** *path* **`-o`** *outfile* { `-f` | `--file-url` | `-u` | `--base-url` | `--ni-url` | `--magnet-url` }

### Example usage:

`dir2ml -d ./MyMirror -u ftp://ftp.example.com -o MyMirror.meta4`

### Required Arguments:

**`-d`**, **`--directory`** *directory* - The directory path to process

**`-o`**, **`--output`** *outfile* - Output filename (`.meta4` or `.metalink`)

If at least one of `-u`/`--base-url`, `-f`/`--file-url`, `--ni-url`, or `--magnet-url` must be supplied.

### Optional Arguments:

**`-c`**, **`--country`** *country-code* - [ISO3166-1 alpha-2](https://datahub.io/core/country-list) two letter country code of the server specified by *base-url* above

**`-h`**, **`--help`** - Show this screen

**`--version`** - Show version information

**`-s`**, **`--show-statistics`** - Show statistics at the end of processing

**`-u`**, **`--base-url`** *base-url* - The base/root URL of an online directory containing the files. For example, `ftp://ftp.example.com`. `dir2ml` will append the relative path of each file to the *base-url*.

**`-f`**, **`--file-url`** - Add a local source for the file, using the directory specified by `--directory` prepended by `file://`. This is useful for fingerprinting a directory or hard drive.

   Note: on Windows, backslashes (`\`) in the *base-url* will be replaced by forward slashes (`/`).

**`-v`**, **`--verbose`** - Verbose output to `stdout`

**`--hash-type`** *hash-list* - Calculate and output all of the hashes specified by *hash-list* (comma-separated). Available hash functions are `md5`, `sha1`, and `sha256`. If none are specified, `sha256` is used.

**`--sparse-output`** - combines `--no-generator` and `--no-date` to simplify diffs

**`--no-generator`** - Don't output the name and version of the tool used to generate the `.meta4` file

**`--no-date`** - Don't output the date the `.meta4` file was generated

**`--ni-url`** - Output Named Information ([RFC6920](https://tools.ietf.org/html/rfc6920)) links (experimental)

**`--magnet-url`** - Output magnet links (experimental)

## Example Output File
```xml
<?xml version="1.0" encoding="UTF-8"?>
<metalink xmlns="urn:ietf:params:xml:ns:metalink" xmlns:nsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="metalink4.xsd">
  <file name="example.ext">
    <size>14471447</size>
    <generator>dir2ml/0.1.0</generator>
    <updated>2010-05-01T12:15:02Z</updated>
    <hash type="sha-256">17bfc4a6058d2d7d82db859c8b0528c6ab48d832fed620ed49fb3385dbf1684d</hash>
    <url location="us" type="ftp">ftp://ftp.example.com/example.ext</url>
  </file>
  <file name="subdir/example2.ext">
    <size>14471447</size>
    <generator>dir2ml/0.1.0</generator>
    <updated>2010-05-01T12:15:02Z</updated>
    <hash type="sha-256">f44bcce2a9c2aa3f73ddc853ad98f87cd8e7cee5b5c18719ebb220da3fd4dbc9</hash>
    <url location="us" type="ftp">ftp://ftp.example.com/subdir/example2.ext</url>
  </file>
</metalink>
```

## Limitations
* Windows only but the code uses only standard C/C++ (no MFC/.NET, etc.) so porting to other operating systems should not be a major task.
* Single threaded so `dir2ml` is CPU-bound, especially when the storage device is fast.
* `dir2ml.exe` may run out of memory when processing a directory containing millions of files because the XML file isn't written until the very end. If you run into this problem, please  [open an issue](https://github.com/hofmand/metalink-builder/issues).
* Only one *base-url* can be specified.
* Only MD5, SHA-1, and SHA-256 hashes are currently supported. *(Do we need any others?)*

## Future Plans
* Port the code to Linux/BSD
* Filter by file size, type, etc.
* Provide a means to merge and split `.meta4` files
* Multithreaded hashing
* Support multiple `country-code`/`base-url` pairs
* Deep-inspect archive files? (`.zip`, `.iso`, etc.)
* Output the `.meta4` file directly to a `.zip` / `.rar` / `.7z` container, in order to save storage space
* Convert [QuickHash](https://quickhash-gui.org/) output files to metalink files if possible (unless the developer decides to [output metalink files directly](https://quickhash-gui.org/bugs/output-in-rfc5854-format/))
* Add [xxHash](https://github.com/Cyan4973/xxHash) and/or [FarmHash](https://github.com/google/farmhash) algorithms

## Also See
* [Hash Archive](https://hash-archive.org/)
* [HashMyFiles](https://www.nirsoft.net/utils/hash_my_files.html)
* [OpenTimestamps](https://opentimestamps.org/)

## Built With
* Microsoft Visual Studio 2015; targeting x64, Unicode

## Authors
Derek Hofmann

## License
This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.