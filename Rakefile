require 'rubygems'
require 'rake'

begin
  require 'jeweler'
  Jeweler::Tasks.new do |gem|
    gem.name = "purple_ruby"
    gem.summary = %q{Gem to send and receive IM messages}
    gem.description = %q{A ruby gem to write server that sends and receives IM messages}
    gem.email = %q{yong@intridea.com}
    gem.homepage = "http://github.com/nofxx/purple_ruby"
    gem.authors = ["yong"]
    gem.add_development_dependency "rspec", ">= 1.2.9"
    # gem is a Gem::Specification... see http://www.rubygems.org/read/chapter/20 for additional settings
  end
  Jeweler::GemcutterTasks.new
rescue LoadError
  puts "Jeweler (or a dependency) not available. Install it with: gem install jeweler"
end

EXT = "ext/ruburple_ext.#{Config::CONFIG['DLEXT']}"

task :default => EXT

file EXT => ["ext/extconf.rb", "ext/purple_ruby_ext.c"] do
  Dir.chdir "ext" do
    ruby "extconf.rb"
    sh "make"
  end
end

=begin
require 'spec/rake/spectask'
Spec::Rake::SpecTask.new(:spec) do |spec|
  spec.libs << 'lib' << 'spec'
  spec.spec_files = FileList['spec/**/*_spec.rb']
end

Spec::Rake::SpecTask.new(:rcov) do |spec|
  spec.libs << 'lib' << 'spec'
  spec.pattern = 'spec/**/*_spec.rb'
  spec.rcov = true
end

task :spec => :check_dependencies
=end

# task :default => :spec

require 'rake/rdoctask'
Rake::RDocTask.new do |rdoc|
  version = File.exist?('VERSION') ? File.read('VERSION') : ""

  rdoc.rdoc_dir = 'rdoc'
  rdoc.title = "purple_ruby #{version}"
  rdoc.rdoc_files.include('README*')
  rdoc.rdoc_files.include('lib/**/*.rb')
  rdoc.rdoc_files.include('ext/**/*.c')
end


