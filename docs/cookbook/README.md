# Cookbook

Each recipe here is one small, complete program that does one common job, the kind of thing you'd otherwise look up, copy and adapt. A recipe makes its own files and starts its own servers, so it runs as it is with no arguments and no network. Every program is under [docs/examples/cookbook](../examples/cookbook), and CI builds and runs each one and checks that it prints what its page says.

## Files and data

- [Add up a column of a CSV file](csv_totals.md)
- [Write a CSV file](csv_write.md)
- [Load a JSON config file](json_config.md)
- [Read a file of JSON lines](json_stream.md)
- [Read an RSS feed](rss_feed.md)
- [Count the status codes in a log](log_status.md)
- [Count the words in a file](word_count.md)
- [Read lines that might be very long](long_lines.md)
- [Pull fields out of text with a regexp](regexp_fields.md)
- [Take a file path apart](file_paths.md)
- [Add up file sizes under a directory](dir_sizes.md)
- [Hash a file with SHA-256](sha256_file.md)
- [Put bytes in text with base64](base64_data.md)
- [Write and read fixed-size binary records](binary_records.md)
- [Spot damaged data with a checksum](crc32_check.md)
- [Compress a file with gzip](gzip_file.md)
- [Zip up a directory](zip_dir.md)
- [Pack files into a .tar.gz](tar_gz.md)
- [Draw a picture and save it as a PNG](png_image.md)
- [Work in a temporary directory](temp_dir.md)
- [Replace a file safely](atomic_write.md)
- [Fill in a text template](template_report.md)
- [Print a table with aligned columns](table_output.md)
- [Sort records by more than one key](sort_records.md)
- [Print a map in a fixed order](sorted_keys.md)

## Text and numbers

- [Work with characters, not bytes](utf8_runes.md)
- [Replace several strings at once](replace_strings.md)
- [Wrap text to a width](word_wrap.md)
- [Do arithmetic on very large integers](big_numbers.md)
- [Make random numbers you can repeat](seeded_random.md)

## Programs and errors

- [Build a tool with subcommands](subcommands.md)
- [Wrap an error and look inside it](wrap_errors.md)
- [Retry a call that fails for a while](retry_backoff.md)
- [Report every problem with some input](validate_form.md)
- [Hide paging behind an iterator](paged_results.md)
- [Read settings from environment variables](env_config.md)
- [Write structured logs as JSON](slog_logs.md)

## Goroutines and time

- [Share work between a few goroutines](worker_pool.md)
- [Run at most a few things at once](limit_concurrency.md)
- [Chain stages with channels](pipeline.md)
- [Share a map between goroutines](mutex_counter.md)
- [Keep a small cache of recent results](lru_cache.md)
- [Take the most urgent job first](priority_queue.md)
- [Give up on slow work](timeout.md)
- [Do something at a steady rate](ticker.md)
- [Parse, format and add times](time_format.md)
- [Parse and print durations](durations.md)

## Networks and HTTP

- [Write a TCP server and client](tcp_echo.md)
- [Check which network an address is in](ip_ranges.md)
- [Serve a directory over HTTP and shut down cleanly](static_files.md)
- [Download a file over HTTP](http_download.md)
- [Read and change a URL's query](url_query.md)
- [Parse email addresses](mail_address.md)
- [Serve a small JSON API](json_api.md)
- [Wrap HTTP handlers with middleware](http_middleware.md)
- [Put untrusted text in a web page](html_safe.md)

## Signing and encryption

- [Store passwords safely](password_hash.md)
- [Sign a message with HMAC](hmac_sign.md)
- [Sign and verify with Ed25519](ed25519_sign.md)
- [Encrypt a message with AES-GCM](aes_gcm.md)
