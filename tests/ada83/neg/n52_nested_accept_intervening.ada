-- Pass 4 negative test: nested accept for same entry across intervening accept (LRM 9.5(8))
procedure N52_Nested_Accept is
   task T is
      entry E1;
      entry E2;
   end T;
   task body T is
   begin
      accept E1 do
         accept E2 do
            accept E1 do
               null;
            end E1;
         end E2;
      end E1;
   end T;
begin
   null;
end N52_Nested_Accept;
