require File.join(File.dirname(__FILE__), %w{.. ext purple_ruby_ext})

Purple = PurpleRuby

Purple::Account.class_eval do
  class AccountOptions
    include Enumerable

    def initialize(acc)
      @acc = acc
    end

    def each
      @acc.protocol.default_options.each{|k,v|
        yield(k, self[k, v])
      }
    end

    def []=(name, value)
      opt = default_value(name)
      case opt
      when Integer
        @acc.set_int_setting(name, value)
      when true, false
        @acc.set_bool_setting(name, value)
      when String
        @acc.set_string_setting(name, value)
      else
        raise "Unrecognized options type #{opt.class}, expected Integer, Boolean or String"
      end
    end

    def [](name, default=nil)
      opt = default || default_value(opt)
      case opt
      when Integer
        @acc.get_int_setting(name, opt)
      when true, false
        @acc.get_bool_setting(name, opt)
      when String
        @acc.get_string_setting(name, opt)
      end
    end

    def default_value(name)
      opt = @acc.protocol.default_options[name.to_s]
      unless opt
        raise "Unknown option #{name}, expected one of: #{@acc.protocol.default_options.keys.join(', ')}"
      end

      opt
    end

    def inspect
      inject("<#AccountOptions: "){|a, (k,v)|
        a << "#{k}=#{v.inspect}, "
      } << "account=#{@acc.inspect}>"
    end
  end

  def options
    @options ||= AccountOptions.new(self)
  end

  def protocol
    id = self.protocol_id
    Purple.list_protocols.find{|p| p.id == id }
  end

end

Purple::Conversation.class_eval do
  def inspect
    "<#Conversation: type=#{type.inspect}, name=#{name.inspect}, title=#{title.inspect}, chat_id=#{chat_id.inspect}}"
  end

  def send_message(message)
    if type == Purple::Conversation::TYPE_CHAT
      chat_send(message)
    else
      im_send(message)
    end
  end
end
