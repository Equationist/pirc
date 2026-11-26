-- Negative test: P3-7.16 for S'Small on subtype & rep clause after derived type freezes parent
procedure Neg_Sub_Small is
   type F is delta 0.25 range -10.0 .. 10.0;
   subtype SF is F range -5.0 .. 5.0;
   for SF'Small use 0.25;
begin
   null;
end Neg_Sub_Small;
