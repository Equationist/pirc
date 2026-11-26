-- Pass 4 negative test: address clause on named number must not ICE in repclau.a
with SYSTEM;
procedure N49_Addr_Named_Num is
   X : Integer := 0;
   N : constant := 1;
   for N use at X'Address;
begin
   null;
end N49_Addr_Named_Num;
