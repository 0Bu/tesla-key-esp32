#!/usr/bin/env ruby
# Real offline YAML syntax gate using Ruby's standard Psych parser. The separate Python workflow
# policy checker owns GitHub-specific DAG, permission, pin, and trust-boundary semantics.
require "tmpdir"
require "yaml"

def validate(paths)
  raise "no YAML files supplied" if paths.empty?
  paths.each do |path|
    document = YAML.parse_file(path)
    raise "#{path}: YAML document is empty" if document.nil?
  rescue Psych::SyntaxError => e
    raise "#{path}: invalid YAML: #{e.message}"
  end
end

self_test = ARGV.delete("--self-test")
begin
  validate(ARGV)
  if self_test
    Dir.mktmpdir("yaml-syntax-selftest-") do |directory|
      malformed = File.join(directory, "malformed.yml")
      File.write(malformed, "jobs:\n  broken: [\n")
      begin
        validate([malformed])
      rescue RuntimeError => e
        raise unless e.message.include?("invalid YAML")
      else
        raise "malformed YAML mutation was accepted"
      end
    end
  end
rescue RuntimeError => e
  warn "yaml-syntax: #{e.message}"
  exit 1
end

puts "yaml-syntax: PASS (#{ARGV.length} files#{self_test ? ", malformed mutation canary" : ""})"
